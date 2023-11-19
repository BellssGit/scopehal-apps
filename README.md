# scopehal-apps

https://www.ngscopeclient.org

Applications for libscopehal

[C++ coding policy](https://github.com/azonenberg/coding-policy/blob/master/cpp-coding-policy.md)

## Installation

Refer to the "getting started" chapter of the User manual
* [User manual GettingStarted (HTML)](https://www.ngscopeclient.org/manual/GettingStarted.html)
* [User manual (PDF)](https://www.ngscopeclient.org/downloads/ngscopeclient-manual.pdf)

## Special comments

The following standard comments are used throughout the code to indicate things that could use attention, but are
not worthy of being tracked as a GitHub issue yet.

* `//TODO`: unimplemented feature, potential optimization point, etc.
* `//FIXME`: known minor problem, temporary workaround, or something that needs to be reworked later
* `//FIXME-CXX20`: places where use of C++ 20 features would simplify the code, but nothing can be done as long as we are targeting platforms which only support C++ 17
