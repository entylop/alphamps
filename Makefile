CXX := clang++
CPPFLAGS := -I.
CXXFLAGS := -O3 -std=c++20 -Wall -Wextra -Wpedantic -pthread
ifeq ($(shell uname -m),arm64)
CXXFLAGS += -mcpu=apple-m3
endif

.PHONY: all test bench sanitize clean
all: intsort

intsort: main.cpp radix_sort.h
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) main.cpp -o $@

sort_tests: tests.cpp radix_sort.h
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tests.cpp -o $@

test: intsort sort_tests
	./sort_tests
	python3 test_cli.py ./intsort

bench: intsort
	./intsort bench --bits 64 --count 4000000

sanitize:
	$(CXX) $(CPPFLAGS) -std=c++20 -O1 -g -pthread -fsanitize=address,undefined tests.cpp -o sort_tests_sanitize
	./sort_tests_sanitize

clean:
	rm -f intsort sort_tests sort_tests_sanitize
	rm -rf sort_tests_sanitize.dSYM
