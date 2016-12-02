# uberlog

uberlog is a cross platform C++ logging system that is:

1. Small
2. Fast
3. Robust
4. Runs on Linux and Windows
5. MIT License

## Small
Two headers, and three source files. Only 2399 lines of code, excluding tests.

## Fast
Logs are written to a shared memory ring buffer. Your program only stalls if the
queue is full, which only happens when the writer cannot keep up with the
amount of log messages. Under such circumstances, you generally have worse
problems (ie unsustainable system load).

## Robust
A child process is spawned, which takes care of writing the log messages to a file.
Even if the main process faults immediately after writing a log message, that last
message (and all the messages before it) will be written into the log file, because
the log writer process will drain the queue, and then exit once it notices that
the main process has died.

Uberlog includes log rolling. You control the maximum size of the log files, and how
many historic log files are kept around.

Uberlog includes type safe formatting that is compatible with printf. See
[tsf](https://github.com/IMQS/tsf) for details on how that works.

## Example
```cpp
#include <uberlog.h>

void example()
{
	uberlog::Logger log;
	log.SetArchiveSettings(50 * 1024 * 1024, 3);    // 3 file history, 50 MB each
	log.SetLevel(uberlog::Level::Info);             // emit logs of level Info or higher
	log.Open("/var/log/mylog");
	log.Info("Starting up");
	log.Warn("Type safe printf %v", "formatting");
}
```

## Extending
The functions built into `Logger` format their output like this:

    Time in server's time zone  Level  Thread   Message
                 |                |      |        |
    2016-11-05T14:28:36.584+0200 [I] 00002c78 The log message

If you want to change that, then you can implement your own logger on
top of uberlog::Logger - either by deriving a class from it, or 
encapsulating it inside your own class. You can then use the LogRaw
function of Logger to emit messages in whatever format you choose.

## Benchmarks

These benchmarks are on an i7-6700K

| OS   |Latency| Throughput |
|------|-------|------------|
Windows| 200 ns| 350 MB/s 
Linux  | 280 ns| 465 MB/s

Those numbers are for a formatted log message that is around 200 bytes long.
The ring buffer copy is around 40 ns, and the buildup of the date string is 130 ns.
The remainder is the formatting of the log message. The ring buffer size affects
the maximum log throughput rate, but the latency is independent of the ring buffer
size.

At a low rate of messages (a few hundred per second), latency is more
important that throughput. To be clear, latency here is the time taken inside
your program to produce the log message and add it to the ring buffer. There
is so much headroom in throughput, that you've probably got worse problems
if you're hitting those limits. 

To put this all into context, let's consider the performance that you get from
a naive logging system that you can easily roll yourself, which simply issues an
OS level `write` call for every log message. What kind of performance do you get
from such a system, and how does it compare to uberlog?

To start with, a kernel call to write 100 bytes to a file is around 1500 ns.
Now let's assume that you don't put much effort into optimizing your formatting
function, or your date string generation, and you end up with another 1000 ns
for the generation of the final log string. Add those together, and you get about
2500 ns per log message.

What does this mean for a typical application? Let's say you're writing 100 log
messages per second. With our hypothetical naive logging system, that amounts to
0.25 milliseconds per second spent logging, which is 0.025% of your time.
If we crank our throughput up to 1000 messages per second, our naive system is
spending 0.25% of total time, just logging, which is still a pretty small number.
At 100000 log messages per second, we start to see some real overhead, with 25%
of our time spent just logging. By using uberlog, that overhead drops to just 2%.

Uberlog takes pains to make all phases of the log write fast, from the type safe
format, to the caching of the date string, to the output into the ring buffer.
That is how we're able to achieve a 10x speedup over a naive system.

When you look at the number above, it becomes quite clear that unless you're
outputting manys thousands of log messages per second, a naive solution is
just fine. But if you want the best, you've come to the right place!
