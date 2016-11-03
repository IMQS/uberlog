# uberlog

uberlog is a cross platform C++ logging system that is:

1. Small
2. Fast
3. Robust
4. Supported on Linux and Windows

## Small
Two headers, and three source files. Only 1980 lines of code, excluding tests.

## Fast
Logs are written to a shared memory ring buffer. A log output only stalls if the
queue is full, which only happens when the writer cannot keep up with the
amount of log messages.

## Robust
A child process is spawned, which takes care of writing the log messages to a file.
Even if the main process faults immediately after writing a log message, that last
message (and all the messages before it) will be written into the log file, because
the log writer process will drain the queue, and then exit once it notices that
the main process has died.

### Ring Buffer Size

The following table is a typical run on an Intel i7-6700K

|RingKB| MsgLen |   KB/s | Msg/s  |
|------|--------|--------|--------|
    64 |    200 |  10746 |   55018
   128 |    200 |  21463 |  109890
   256 |    200 |  42719 |  218723
   512 |    200 |  83610 |  428082
  1024 |    200 | 147073 |  753012
  2048 |    200 | 290644 | 1488095
  4096 |    200 | 325521 | 1666667
  8192 |    200 | 356410 | 1824818

RingKB is the size of the ring buffer in kilobytes.  
MsgLen is the length of each log message.  
KB/s is the throughput in terms of total kilobytes written to the log file.  
Msg/s is the throughput in terms of number of messages written to the log file. 

uberlog is designed to be a regular human readable logging system, which implies
a certain frequency of messages. If we say that messages are on average 200 bytes
long, and we pick a ring buffer size of 1024 KB, then we see from the above table
that we can sustain about 750,000 messages per second. Such a high volume of log
messages would quickly saturate typical log indexing systems, or third party
logging aggregators, so we're at 700k msg/s, we're far into the "good enough"
performance territory.  
What's perhaps more pertinent is the burst rate. When the log slave writer sees
no incoming messages, it gradually raises it's sleep time, from 1 ms, up to 1000 ms.
If the slave is sleeping for one second, then we want to be able to emit a burst of
messages during that one second, and not stall, waiting for the slave to wake up.
With a ring buffer size of 1 MB, the buffer can absord 5000 messages of 200 bytes.
This seems like a reasonable burst rate for most applications. Of course, if you
want to raise the ring size, you can do that.  
One other thing that's very releant in choosing a ring buffer size, is the maximum
size of a log message. Any single log message that is longer than the ring buffer,
is truncated to the ring buffer length.