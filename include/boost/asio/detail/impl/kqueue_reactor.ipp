//
// detail/impl/kqueue_reactor.ipp
// ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
// Copyright (c) 2003-2011 Christopher M. Kohlhoff (chris at kohlhoff dot com)
// Copyright (c) 2005 Stefan Arentz (stefan at soze dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef BOOST_ASIO_DETAIL_IMPL_KQUEUE_REACTOR_IPP
#define BOOST_ASIO_DETAIL_IMPL_KQUEUE_REACTOR_IPP

#if defined(_MSC_VER) && (_MSC_VER >= 1200)
# pragma once
#endif // defined(_MSC_VER) && (_MSC_VER >= 1200)

#include <boost/asio/detail/config.hpp>

#if defined(BOOST_ASIO_HAS_KQUEUE)

#include <boost/asio/detail/kqueue_reactor.hpp>
#include <boost/asio/detail/throw_error.hpp>
#include <boost/asio/error.hpp>

#include <boost/asio/detail/push_options.hpp>

#if defined(__NetBSD__)
# define BOOST_ASIO_KQUEUE_EV_SET(ev, ident, filt, flags, fflags, data, udata) \
    EV_SET(ev, ident, filt, flags, fflags, \
      data, reinterpret_cast<intptr_t>(udata))
#else
# define BOOST_ASIO_KQUEUE_EV_SET(ev, ident, filt, flags, fflags, data, udata) \
    EV_SET(ev, ident, filt, flags, fflags, data, udata)
#endif

namespace boost {
namespace asio {
namespace detail {

kqueue_reactor::kqueue_reactor(boost::asio::io_service& io_service)
  : boost::asio::detail::service_base<kqueue_reactor>(io_service),
    io_service_(use_service<io_service_impl>(io_service)),
    mutex_(),
    kqueue_fd_(do_kqueue_create()),
    interrupter_(),
    shutdown_(false)
{
  // The interrupter is put into a permanently readable state. Whenever we
  // want to interrupt the blocked kevent call we register a one-shot read
  // operation against the descriptor.
  interrupter_.interrupt();
}

kqueue_reactor::~kqueue_reactor()
{
  close(kqueue_fd_);
}

void kqueue_reactor::shutdown_service()
{
  mutex::scoped_lock lock(mutex_);
  shutdown_ = true;
  lock.unlock();

  op_queue<operation> ops;

  while (descriptor_state* state = registered_descriptors_.first())
  {
    for (int i = 0; i < max_ops; ++i)
      ops.push(state->op_queue_[i]);
    state->shutdown_ = true;
    registered_descriptors_.free(state);
  }

  timer_queues_.get_all_timers(ops);
}

void kqueue_reactor::init_task()
{
  io_service_.init_task();
}

int kqueue_reactor::register_descriptor(socket_type,
    kqueue_reactor::per_descriptor_data& descriptor_data)
{
  mutex::scoped_lock lock(registered_descriptors_mutex_);

  descriptor_data = registered_descriptors_.alloc();
  descriptor_data->shutdown_ = false;

  return 0;
}

void kqueue_reactor::start_op(int op_type, socket_type descriptor,
    kqueue_reactor::per_descriptor_data& descriptor_data,
    reactor_op* op, bool allow_speculative)
{
  if (!descriptor_data)
  {
    op->ec_ = boost::asio::error::bad_descriptor;
    post_immediate_completion(op);
    return;
  }

  mutex::scoped_lock descriptor_lock(descriptor_data->mutex_);

  if (descriptor_data->shutdown_)
  {
    post_immediate_completion(op);
    return;
  }

  bool first = descriptor_data->op_queue_[op_type].empty();
  if (first)
  {
    if (allow_speculative)
    {
      if (op_type != read_op || descriptor_data->op_queue_[except_op].empty())
      {
        if (op->perform())
        {
          descriptor_lock.unlock();
          io_service_.post_immediate_completion(op);
          return;
        }
      }
    }
  }

  descriptor_data->op_queue_[op_type].push(op);
  io_service_.work_started();

  if (first)
  {
    struct kevent event;
    switch (op_type)
    {
    case read_op:
      BOOST_ASIO_KQUEUE_EV_SET(&event, descriptor, EVFILT_READ,
          EV_ADD | EV_ONESHOT, 0, 0, descriptor_data);
      break;
    case write_op:
      BOOST_ASIO_KQUEUE_EV_SET(&event, descriptor, EVFILT_WRITE,
          EV_ADD | EV_ONESHOT, 0, 0, descriptor_data);
      break;
    case except_op:
      if (!descriptor_data->op_queue_[read_op].empty())
        return; // Already registered for read events.
      BOOST_ASIO_KQUEUE_EV_SET(&event, descriptor, EVFILT_READ,
          EV_ADD | EV_ONESHOT, EV_OOBAND, 0, descriptor_data);
      break;
    }

    if (::kevent(kqueue_fd_, &event, 1, 0, 0, 0) == -1)
    {
      op->ec_ = boost::system::error_code(errno,
          boost::asio::error::get_system_category());
      descriptor_data->op_queue_[op_type].pop();
      io_service_.post_deferred_completion(op);
    }
  }
}

void kqueue_reactor::cancel_ops(socket_type,
    kqueue_reactor::per_descriptor_data& descriptor_data)
{
  if (!descriptor_data)
    return;

  mutex::scoped_lock descriptor_lock(descriptor_data->mutex_);

  op_queue<operation> ops;
  for (int i = 0; i < max_ops; ++i)
  {
    while (reactor_op* op = descriptor_data->op_queue_[i].front())
    {
      op->ec_ = boost::asio::error::operation_aborted;
      descriptor_data->op_queue_[i].pop();
      ops.push(op);
    }
  }

  descriptor_lock.unlock();

  io_service_.post_deferred_completions(ops);
}

void kqueue_reactor::close_descriptor(socket_type,
    kqueue_reactor::per_descriptor_data& descriptor_data)
{
  if (!descriptor_data)
    return;

  mutex::scoped_lock descriptor_lock(descriptor_data->mutex_);
  mutex::scoped_lock descriptors_lock(registered_descriptors_mutex_);

  if (!descriptor_data->shutdown_)
  {
    // Remove the descriptor from the set of known descriptors. The descriptor
    // will be automatically removed from the kqueue set when it is closed.

    op_queue<operation> ops;
    for (int i = 0; i < max_ops; ++i)
    {
      while (reactor_op* op = descriptor_data->op_queue_[i].front())
      {
        op->ec_ = boost::asio::error::operation_aborted;
        descriptor_data->op_queue_[i].pop();
        ops.push(op);
      }
    }

    descriptor_data->shutdown_ = true;

    descriptor_lock.unlock();

    registered_descriptors_.free(descriptor_data);
    descriptor_data = 0;

    descriptors_lock.unlock();

    io_service_.post_deferred_completions(ops);
  }
}

void kqueue_reactor::run(bool block, op_queue<operation>& ops)
{
  mutex::scoped_lock lock(mutex_);

  // Determine how long to block while waiting for events.
  timespec timeout_buf = { 0, 0 };
  timespec* timeout = block ? get_timeout(timeout_buf) : &timeout_buf;

  lock.unlock();

  // Block on the kqueue descriptor.
  struct kevent events[128];
  int num_events = kevent(kqueue_fd_, 0, 0, events, 128, timeout);

  // Dispatch the waiting events.
  for (int i = 0; i < num_events; ++i)
  {
    int descriptor = events[i].ident;
    void* ptr = reinterpret_cast<void*>(events[i].udata);
    if (ptr == &interrupter_)
    {
      // No need to reset the interrupter since we're leaving the descriptor
      // in a ready-to-read state and relying on one-shot notifications.
    }
    else
    {
      descriptor_state* descriptor_data = static_cast<descriptor_state*>(ptr);
      mutex::scoped_lock descriptor_lock(descriptor_data->mutex_);

      // Exception operations must be processed first to ensure that any
      // out-of-band data is read before normal data.
#if defined(__NetBSD__)
      static const unsigned int filter[max_ops] =
#else
      static const int filter[max_ops] =
#endif
        { EVFILT_READ, EVFILT_WRITE, EVFILT_READ };
      for (int j = max_ops - 1; j >= 0; --j)
      {
        if (events[i].filter == filter[j])
        {
          if (j != except_op || events[i].flags & EV_OOBAND)
          {
            while (reactor_op* op = descriptor_data->op_queue_[j].front())
            {
              if (events[i].flags & EV_ERROR)
              {
                op->ec_ = boost::system::error_code(events[i].data,
                    boost::asio::error::get_system_category());
                descriptor_data->op_queue_[j].pop();
                ops.push(op);
              }
              if (op->perform())
              {
                descriptor_data->op_queue_[j].pop();
                ops.push(op);
              }
              else
                break;
            }
          }
        }
      }

      // Renew registration for event notifications.
      struct kevent event;
      switch (events[i].filter)
      {
      case EVFILT_READ:
        if (!descriptor_data->op_queue_[read_op].empty())
          BOOST_ASIO_KQUEUE_EV_SET(&event, descriptor, EVFILT_READ,
              EV_ADD | EV_ONESHOT, 0, 0, descriptor_data);
        else if (!descriptor_data->op_queue_[except_op].empty())
          BOOST_ASIO_KQUEUE_EV_SET(&event, descriptor, EVFILT_READ,
              EV_ADD | EV_ONESHOT, EV_OOBAND, 0, descriptor_data);
        else
          continue;
      case EVFILT_WRITE:
        if (!descriptor_data->op_queue_[write_op].empty())
          BOOST_ASIO_KQUEUE_EV_SET(&event, descriptor, EVFILT_WRITE,
              EV_ADD | EV_ONESHOT, 0, 0, descriptor_data);
        else
          continue;
      default:
        break;
      }
      if (::kevent(kqueue_fd_, &event, 1, 0, 0, 0) == -1)
      {
        boost::system::error_code error(errno,
            boost::asio::error::get_system_category());
        for (int j = 0; j < max_ops; ++j)
        {
          while (reactor_op* op = descriptor_data->op_queue_[j].front())
          {
            op->ec_ = error;
            descriptor_data->op_queue_[j].pop();
            ops.push(op);
          }
        }
      }
    }
  }

  lock.lock();
  timer_queues_.get_ready_timers(ops);
}

void kqueue_reactor::interrupt()
{
  struct kevent event;
  BOOST_ASIO_KQUEUE_EV_SET(&event, interrupter_.read_descriptor(),
      EVFILT_READ, EV_ADD | EV_ONESHOT, 0, 0, &interrupter_);
  ::kevent(kqueue_fd_, &event, 1, 0, 0, 0);
}

int kqueue_reactor::do_kqueue_create()
{
  int fd = ::kqueue();
  if (fd == -1)
  {
    boost::system::error_code ec(errno,
        boost::asio::error::get_system_category());
    boost::asio::detail::throw_error(ec, "kqueue");
  }
  return fd;
}

void kqueue_reactor::do_add_timer_queue(timer_queue_base& queue)
{
  mutex::scoped_lock lock(mutex_);
  timer_queues_.insert(&queue);
}

void kqueue_reactor::do_remove_timer_queue(timer_queue_base& queue)
{
  mutex::scoped_lock lock(mutex_);
  timer_queues_.erase(&queue);
}

timespec* kqueue_reactor::get_timeout(timespec& ts)
{
  // By default we will wait no longer than 5 minutes. This will ensure that
  // any changes to the system clock are detected after no longer than this.
  long usec = timer_queues_.wait_duration_usec(5 * 60 * 1000 * 1000);
  ts.tv_sec = usec / 1000000;
  ts.tv_nsec = (usec % 1000000) * 1000;
  return &ts;
}

} // namespace detail
} // namespace asio
} // namespace boost

#undef BOOST_ASIO_KQUEUE_EV_SET

#include <boost/asio/detail/pop_options.hpp>

#endif // defined(BOOST_ASIO_HAS_KQUEUE)

#endif // BOOST_ASIO_DETAIL_IMPL_KQUEUE_REACTOR_IPP
