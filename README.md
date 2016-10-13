# uberlog

uberlog is a cross platform C++ logging system that is

1. Small
2. Fast
3. Robust
4. Runs on Linux and Windows

## Small
One header, and two source files. Only XXX lines of code.

## Fast
Logs are written to a shared memory ring buffer. A log output only stalls if the
queue is full, which only happens if the volume of logs exceeds the IO system's
write bandwidth.

## Robust
A child process is spawned, which takes care of writing the log messages to a file.
Even if the main process faults immediately after writing a log message, that last
message (and all the messages before it) will be written into the log file, because
the log writer process will consume the queue, and then exit once it notices that
the main process has died.
