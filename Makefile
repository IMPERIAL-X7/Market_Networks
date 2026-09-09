# The Socket Exchange.
#
# This Makefile is a thin wrapper so that "make" does the expected thing. The
# real build is tools/build.sh, because FreeBSD's make(1) is BSD make while
# GNU make is only a port: one Makefile cannot use the conveniences of either
# without breaking under the other. Only portable constructs appear here, so
# this file works with both.
#
#   make                 build everything
#   make test            build, then run the protocol conformance tests
#   make clean           remove build products
#   make submission ROLLS="ROLL1 ROLL2"

all:
	@sh tools/build.sh all

server:
	@sh tools/build.sh exchange_server

clients:
	@sh tools/build.sh trader_client market_data_client

test: all check-kqueue
	python3 tests/test_protocol.py

# The kqueue backend is used on FreeBSD and cannot be built on Linux. Away
# from FreeBSD this compiles it against a stand-in for <sys/event.h> that
# matches FreeBSD's declarations, so its API usage is still checked.
check-kqueue:
	@sh tools/check_kqueue.sh

submission:
	@sh tools/make_submission.sh $(ROLLS)

clean:
	@sh tools/build.sh clean

.PHONY: all server clients test check-kqueue submission clean
