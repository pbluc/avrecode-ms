# avrecode: lossless re-compression of compressed video streams

## Linux Installation

Download the source:

```
git clone https://github.com/dropbox/avrecode
cd avrecode
git submodule update --init
```

Build and test:

```
sudo apt-get update
sudo apt-get install protobuf-compiler build-essential yasm pkg-config
cd ffmpeg
./configure
make
cd ..
make
./recode roundtrip data/GOPR4542.MP4
```

Warning
-------
This is an experimental test bed for compression research: use on trusted inputs only
This tool does not validate input.

License
-------

avrecode is released under the BSD 3-clause license. See the LICENSE file for details.
The required libavcodec-hooks patch to ffmpeg is licenced under the LGPL.


Contributing
------------

avrecode was originally written by Chris Lesniewski during Dropbox Hack Week
January 2016. It is a redesign of the first version written by Daniel Horn,
Patrick Horn, Chris Lesniewski, and others during Dropbox Hack Week 2015.
We welcome external contributions, but ask that contributors accept our
Contributor License Agreement to grant us a license to distribute the code:

https://opensource.dropbox.com/cla/
