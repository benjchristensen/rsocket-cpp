# reactivesocket-cpp
C++ implementation of ReactiveSocket

# Build Status

<a href='https://travis-ci.org/ReactiveSocket/reactivesocket-cpp/builds'><img src='https://travis-ci.org/ReactiveSocket/reactivesocket-cpp.svg?branch=master'></a>

# Dependencies

Install `folly`:

```
brew install folly
```

After first checkout, initialize and update submodules:

```
git submodule init
git submodule update --recursive
```

# Building and running tests

After installing dependencies as above, you can build and run tests with:

```
mkdir -p build
cd build
cmake ../
make
./ReactiveSocketTest
```
