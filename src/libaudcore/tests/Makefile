all: test test-mainloop-glib test-mainloop-qt

SRCS = ../audstrings.cc \
       ../charset.cc \
       ../hook.cc \
       ../index.cc \
       ../multihash.cc \
       ../stringbuf.cc \
       ../strpool.cc \
       ../tinylock.cc \
       ../tuple.cc \
       ../tuple-compiler.cc \
       test.cc

FLAGS = -I.. -I../.. -DEXPORT= -DPACKAGE=\"audacious\" -DICONV_CONST= \
        $(shell pkg-config --cflags --libs glib-2.0) \
        -std=c++11 -Wall -g -O0 -fno-elide-constructors \
        -fprofile-arcs -ftest-coverage -pthread

MAINLOOP_SRCS = ../mainloop.cc test-mainloop.cc

test: ${SRCS}
	g++ ${SRCS} ${FLAGS} -o test

test-mainloop-glib: ${MAINLOOP_SRCS}
	g++ ${MAINLOOP_SRCS} ${FLAGS} -o test-mainloop-glib

test-mainloop-qt: ${MAINLOOP_SRCS}
	g++ ${MAINLOOP_SRCS} ${FLAGS} -DUSE_QT -fPIC \
	$(shell pkg-config --cflags --libs Qt5Core) \
	-o test-mainloop-qt

cov: all
	rm -f *.gcda
	./test
	./test-mainloop-glib
	./test-mainloop-qt
	gcov --object-directory . ${SRCS} ${MAINLOOP_SRCS}

clean:
	rm -f test test-mainloop-glib test-mainloop-qt *.gcno *.gcda *.gcov
