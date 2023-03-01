# wavestream

notes about building:
* cmake needs to "find" the libsndfile package, but the usual file that would do this seems to be a built output from libsndfile - so for everything to really work you probably need to build libsndfile first (in the libsndfile root: mkdir build; cd build; cmake ..; cmake --build .)


# usage

## wavstream
```Usage : wavstream <input file>```

^^^ start a server which repeatedly streams the WAV file pointed to by \<input file\> to connections on port 42310
* note: the sample rate is not transmitted, so it is impossible to get the exact precise frequency content of the stream on the receiving side. but hey, we just want it to look pretty for a bit I think.

## client

```usage ./client hostname port```
```./client localhost 42310```

^^^ start a client which connects to the wavstream server to handle streamed audio frames.
