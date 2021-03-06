/*
 * Copyright (c) 2011 Tony Arcieri. Distributed under the MIT License. See
 * LICENSE.txt for further details.
 */

#include "nio4r.h"
#include "rubysig.h"
#include <unistd.h>
#include <fcntl.h>
#include <assert.h>

static VALUE mNIO = Qnil;
static VALUE cNIO_Monitor  = Qnil;
static VALUE cNIO_Selector = Qnil;

/* Allocator/deallocator */
static VALUE NIO_Selector_allocate(VALUE klass);
static void NIO_Selector_mark(struct NIO_Selector *loop);
static void NIO_Selector_shutdown(struct NIO_Selector *selector);
static void NIO_Selector_free(struct NIO_Selector *loop);

/* Methods */
static VALUE NIO_Selector_initialize(VALUE self);
static VALUE NIO_Selector_register(VALUE self, VALUE selectable, VALUE interest);
static VALUE NIO_Selector_deregister(VALUE self, VALUE io);
static VALUE NIO_Selector_is_registered(VALUE self, VALUE io);
static VALUE NIO_Selector_select(int argc, VALUE *argv, VALUE self);
static VALUE NIO_Selector_wakeup(VALUE self);
static VALUE NIO_Selector_close(VALUE self);
static VALUE NIO_Selector_closed(VALUE self);

/* Internal functions */
static VALUE NIO_Selector_synchronize(VALUE self, VALUE (*func)(VALUE *args), VALUE *args);
static VALUE NIO_Selector_unlock(VALUE lock);
static VALUE NIO_Selector_register_synchronized(VALUE *args);
static VALUE NIO_Selector_deregister_synchronized(VALUE *args);
static VALUE NIO_Selector_select_synchronized(VALUE *args);
static int NIO_Selector_run(struct NIO_Selector *selector, VALUE timeout);
static void NIO_Selector_timeout_callback(struct ev_loop *ev_loop, struct ev_timer *timer, int revents);
static void NIO_Selector_wakeup_callback(struct ev_loop *ev_loop, struct ev_io *io, int revents);

/* Default number of slots in the buffer for selected monitors */
#define INITIAL_READY_BUFFER 32

/* Ruby 1.8 needs us to busy wait and run the green threads scheduler every 10ms */
#define BUSYWAIT_INTERVAL 0.01

/* Selectors wait for events */
void Init_NIO_Selector()
{
    mNIO = rb_define_module("NIO");
    cNIO_Selector = rb_define_class_under(mNIO, "Selector", rb_cObject);
    rb_define_alloc_func(cNIO_Selector, NIO_Selector_allocate);

    rb_define_method(cNIO_Selector, "initialize", NIO_Selector_initialize, 0);
    rb_define_method(cNIO_Selector, "register", NIO_Selector_register, 2);
    rb_define_method(cNIO_Selector, "deregister", NIO_Selector_deregister, 1);
    rb_define_method(cNIO_Selector, "registered?", NIO_Selector_is_registered, 1);
    rb_define_method(cNIO_Selector, "select", NIO_Selector_select, -1);
    rb_define_method(cNIO_Selector, "wakeup", NIO_Selector_wakeup, 0);
    rb_define_method(cNIO_Selector, "close", NIO_Selector_close, 0);
    rb_define_method(cNIO_Selector, "closed?", NIO_Selector_closed, 0);

    cNIO_Monitor = rb_define_class_under(mNIO, "Monitor",  rb_cObject);
}

/* Create the libev event loop and incoming event buffer */
static VALUE NIO_Selector_allocate(VALUE klass)
{
    struct NIO_Selector *selector;
    int fds[2];

    /* Use a pipe to implement the wakeup mechanism. I know libev provides
       async watchers that implement this same behavior, but I'm getting
       segvs trying to use that between threads, despite claims of thread
       safety. Pipes are nice and safe to use between threads.

       Note that Java NIO uses this same mechanism */
    if(pipe(fds) < 0) {
        rb_sys_fail("pipe");
    }

    if(fcntl(fds[0], F_SETFL, O_NONBLOCK) < 0) {
        rb_sys_fail("fcntl");
    }

    selector = (struct NIO_Selector *)xmalloc(sizeof(struct NIO_Selector));
    selector->ev_loop = ev_loop_new(0);
    ev_init(&selector->timer, NIO_Selector_timeout_callback);

    selector->wakeup_reader = fds[0];
    selector->wakeup_writer = fds[1];

    ev_io_init(&selector->wakeup, NIO_Selector_wakeup_callback, selector->wakeup_reader, EV_READ);
    selector->wakeup.data = (void *)selector;
    ev_io_start(selector->ev_loop, &selector->wakeup);

    selector->closed = selector->selecting = selector->ready_count = 0;
    selector->ready_array = Qnil;

    return Data_Wrap_Struct(klass, NIO_Selector_mark, NIO_Selector_free, selector);
}

/* NIO selectors store all Ruby objects in instance variables so mark is a stub */
static void NIO_Selector_mark(struct NIO_Selector *selector)
{
    if(selector->ready_array != Qnil) {
        rb_gc_mark(selector->ready_array);
    }
}

/* Free a Selector's system resources.
   Called by both NIO::Selector#close and the finalizer below */
static void NIO_Selector_shutdown(struct NIO_Selector *selector)
{
    if(selector->ev_loop) {
        ev_loop_destroy(selector->ev_loop);
        selector->ev_loop = 0;
    }

    if(selector->closed) {
        return;
    }

    close(selector->wakeup_reader);
    close(selector->wakeup_writer);

    selector->closed = 1;
}

/* Ruby finalizer for selector objects */
static void NIO_Selector_free(struct NIO_Selector *selector)
{
    NIO_Selector_shutdown(selector);
    xfree(selector);
}

/* Create a new selector. This is more or less the pure Ruby version
   translated into an MRI cext */
static VALUE NIO_Selector_initialize(VALUE self)
{
    VALUE lock;

    rb_ivar_set(self, rb_intern("selectables"), rb_hash_new());

    lock = rb_class_new_instance(0, 0, rb_const_get(rb_cObject, rb_intern("Mutex")));
    rb_ivar_set(self, rb_intern("lock"), lock);

    return Qnil;
}

/* Synchronize around a reentrant selector lock */
static VALUE NIO_Selector_synchronize(VALUE self, VALUE (*func)(VALUE *args), VALUE *args)
{
    VALUE current_thread, lock_holder, lock;

    current_thread = rb_thread_current();
    lock_holder = rb_ivar_get(self, rb_intern("lock_holder"));

    if(lock_holder != rb_thread_current()) {
        lock = rb_ivar_get(self, rb_intern("lock"));
        rb_funcall(lock, rb_intern("lock"), 0, 0);
        rb_ivar_set(self, rb_intern("lock_holder"), rb_thread_current());

        /* We've acquired the lock, so ensure we unlock it */
        return rb_ensure(func, (VALUE)args, NIO_Selector_unlock, self);
    } else {
        /* We already hold the selector lock, so no need to unlock it */
        func(args);
    }
}

/* Unlock the selector mutex */
static VALUE NIO_Selector_unlock(VALUE self)
{
    VALUE lock;

    rb_ivar_set(self, rb_intern("lock_holder"), Qnil);

    lock = rb_ivar_get(self, rb_intern("lock"));
    rb_funcall(lock, rb_intern("unlock"), 0, 0);
}

/* Register an IO object with the selector for the given interests */
static VALUE NIO_Selector_register(VALUE self, VALUE io, VALUE interests)
{
    VALUE args[3] = {self, io, interests};
    return NIO_Selector_synchronize(self, NIO_Selector_register_synchronized, args);
}

/* Internal implementation of register after acquiring mutex */
static VALUE NIO_Selector_register_synchronized(VALUE *args)
{
    VALUE self, io, interests, selectables, monitor;
    VALUE monitor_args[3];

    self = args[0];
    io = args[1];
    interests = args[2];

    selectables = rb_ivar_get(self, rb_intern("selectables"));
    monitor = rb_hash_lookup(selectables, io);

    if(monitor != Qnil)
        rb_raise(rb_eArgError, "this IO is already registered with selector");

    /* Create a new NIO::Monitor */
    monitor_args[0] = io;
    monitor_args[1] = interests;
    monitor_args[2] = self;

    monitor = rb_class_new_instance(3, monitor_args, cNIO_Monitor);
    rb_hash_aset(selectables, io, monitor);

    return monitor;
}

/* Deregister an IO object from the selector */
static VALUE NIO_Selector_deregister(VALUE self, VALUE io)
{
    VALUE args[2] = {self, io};
    return NIO_Selector_synchronize(self, NIO_Selector_deregister_synchronized, args);
}

/* Internal implementation of register after acquiring mutex */
static VALUE NIO_Selector_deregister_synchronized(VALUE *args)
{
    VALUE self, io, interests, selectables, monitor;
    VALUE monitor_args[3];

    self = args[0];
    io = args[1];

    selectables = rb_ivar_get(self, rb_intern("selectables"));
    monitor = rb_hash_delete(selectables, io);

    if(monitor != Qnil) {
        rb_funcall(monitor, rb_intern("close"), 1, Qfalse);
    }

    return monitor;
}

/* Is the given IO object registered with the selector */
static VALUE NIO_Selector_is_registered(VALUE self, VALUE io)
{
    VALUE selectables = rb_ivar_get(self, rb_intern("selectables"));

    /* Perhaps this should be holding the mutex? */
    return rb_funcall(selectables, rb_intern("has_key?"), 1, io);
}

/* Select from all registered IO objects */
static VALUE NIO_Selector_select(int argc, VALUE *argv, VALUE self)
{
    VALUE timeout, array;
    VALUE args[2];

    rb_scan_args(argc, argv, "01", &timeout);

    if(timeout != Qnil && NUM2DBL(timeout) < 0) {
        rb_raise(rb_eArgError, "time interval must be positive");
    }

    args[0] = self;
    args[1] = timeout;

    return NIO_Selector_synchronize(self, NIO_Selector_select_synchronized, args);
}

/* Internal implementation of select with the selector lock held */
static VALUE NIO_Selector_select_synchronized(VALUE *args)
{
    int i, ready;
    VALUE ready_array;
    struct NIO_Selector *selector;

    Data_Get_Struct(args[0], struct NIO_Selector, selector);
    if(!rb_block_given_p()) {
        selector->ready_array = rb_ary_new();
    }

    ready = NIO_Selector_run(selector, args[1]);
    if(ready > 0) {
        if(rb_block_given_p()) {
            return INT2NUM(ready);
        } else {
            ready_array = selector->ready_array;
            selector->ready_array = Qnil;
            return ready_array;
        }
    } else {
        selector->ready_array = Qnil;
        return Qnil;
    }
}

static int NIO_Selector_run(struct NIO_Selector *selector, VALUE timeout)
{
    int result;
    selector->selecting = 1;

#if defined(HAVE_RB_THREAD_BLOCKING_REGION) || defined(HAVE_RB_THREAD_ALONE)
    /* Implement the optional timeout (if any) as a ev_timer */
    if(timeout != Qnil) {
        /* It seems libev is not a fan of timers being zero, so fudge a little */
        selector->timer.repeat = NUM2DBL(timeout) + 0.0001;
        ev_timer_again(selector->ev_loop, &selector->timer);
    } else {
        ev_timer_stop(selector->ev_loop, &selector->timer);
    }
#else
    /* Store when we started the loop so we can calculate the timeout */
    ev_tstamp started_at = ev_now(selector->ev_loop);
#endif

#if defined(HAVE_RB_THREAD_BLOCKING_REGION)
    /* libev is patched to release the GIL when it makes its system call */
    ev_loop(selector->ev_loop, EVLOOP_ONESHOT);
#elif defined(HAVE_RB_THREAD_ALONE)
    /* If we're the only thread we can make a blocking system call */
    if(rb_thread_alone()) {
#else
    /* If we don't have rb_thread_alone() we can't block */
    if(0) {
#endif /* defined(HAVE_RB_THREAD_BLOCKING_REGION) */

#if !defined(HAVE_RB_THREAD_BLOCKING_REGION)
        TRAP_BEG;
        ev_loop(selector->ev_loop, EVLOOP_ONESHOT);
        TRAP_END;
    } else {
        /* We need to busy wait as not to stall the green thread scheduler
           Ruby 1.8: just say no! :( */
        ev_timer_init(&selector->timer, NIO_Selector_timeout_callback, BUSYWAIT_INTERVAL, BUSYWAIT_INTERVAL);
        ev_timer_start(selector->ev_loop, &selector->timer);

        /* Loop until we receive events */
        while(selector->selecting && !selector->ready_count) {
            TRAP_BEG;
            ev_loop(selector->ev_loop, EVLOOP_ONESHOT);
            TRAP_END;

            /* Run the next green thread */
            rb_thread_schedule();

            /* Break if the timeout has elapsed */
            if(timeout != Qnil && ev_now(selector->ev_loop) - started_at >= NUM2DBL(timeout))
                break;
        }

        ev_timer_stop(selector->ev_loop, &selector->timer);
    }
#endif /* defined(HAVE_RB_THREAD_BLOCKING_REGION) */

    result = selector->ready_count;
    selector->selecting = selector->ready_count = 0;

    return result;
}

/* Wake the selector up from another thread */
static VALUE NIO_Selector_wakeup(VALUE self)
{
    struct NIO_Selector *selector;
    Data_Get_Struct(self, struct NIO_Selector, selector);

    if(selector->closed) {
        rb_raise(rb_eIOError, "selector is closed");
    }

    write(selector->wakeup_writer, "\0", 1);
    return Qnil;
}

/* Close the selector and free system resources */
static VALUE NIO_Selector_close(VALUE self)
{
    struct NIO_Selector *selector;
    Data_Get_Struct(self, struct NIO_Selector, selector);

    NIO_Selector_shutdown(selector);

    return Qnil;
}

/* Is the selector closed? */
static VALUE NIO_Selector_closed(VALUE self)
{
    struct NIO_Selector *selector;
    Data_Get_Struct(self, struct NIO_Selector, selector);

    return selector->closed ? Qtrue : Qfalse;
}

/* Called whenever a timeout fires on the event loop */
static void NIO_Selector_timeout_callback(struct ev_loop *ev_loop, struct ev_timer *timer, int revents)
{
    /* We don't actually need to do anything here, the mere firing of the
       timer is sufficient to interrupt the selector. However, libev still wants a callback */
}

/* Called whenever a wakeup request is sent to a selector */
static void NIO_Selector_wakeup_callback(struct ev_loop *ev_loop, struct ev_io *io, int revents)
{
    char buffer[128];
    struct NIO_Selector *selector = (struct NIO_Selector *)io->data;
    selector->selecting = 0;

    /* Drain the wakeup pipe, giving us level-triggered behavior */
    while(read(selector->wakeup_reader, buffer, 128) > 0);
}

/* libev callback fired whenever a monitor gets an event */
void NIO_Selector_monitor_callback(struct ev_loop *ev_loop, struct ev_io *io, int revents)
{
    struct NIO_Monitor *monitor_data = (struct NIO_Monitor *)io->data;
    struct NIO_Selector *selector = monitor_data->selector;
    VALUE monitor = monitor_data->self;

    assert(selector != 0);
    selector->ready_count++;
    monitor_data->revents = revents;

    if(rb_block_given_p()) {
        rb_yield(monitor);
    } else {
        assert(selector->ready_array != Qnil);
        rb_ary_push(selector->ready_array, monitor);
    }
}
