# Host build (Linux/macOS) of the Amatrix client prototype.
# Needs libcurl development files and a C++ compiler for libolm.
#
#   make            -> build/amatrix-recv
#   make clean

OLM   = ../third_party/libolm
CJSON = ../third_party/cjson
B     = build

CC  ?= cc
CXX ?= c++
CFLAGS   ?= -O2 -g -Wall
CXXFLAGS ?= -O2 -g
OLM_CPPFLAGS = -I$(OLM)/include -I$(OLM)/lib -DOLMLIB_VERSION_MAJOR=3 -DOLMLIB_VERSION_MINOR=2 -DOLMLIB_VERSION_PATCH=16 -DOLM_STATIC_DEFINE

OLM_CXX = $(wildcard $(OLM)/src/*.cpp)
OLM_C   = $(wildcard $(OLM)/src/*.c) $(OLM)/lib/crypto-algorithms/aes.c \
          $(OLM)/lib/crypto-algorithms/sha256.c $(OLM)/lib/curve25519-donna/curve25519-donna.c
OLM_OBJ = $(patsubst $(OLM)/%,$(B)/olm/%.o,$(OLM_CXX) $(OLM_C))

APP_SRC = src/recv.c src/canonjson.c src/http_curl.c src/platform_posix.c $(CJSON)/cJSON.c
APP_OBJ = $(patsubst %,$(B)/app/%.o,$(notdir $(APP_SRC)))

all: $(B)/amatrix-recv

$(B)/amatrix-recv: $(APP_OBJ) $(B)/libolm.a
	$(CXX) -o $@ $(APP_OBJ) $(B)/libolm.a $$(pkg-config --libs libcurl 2>/dev/null || echo -lcurl)

$(B)/libolm.a: $(OLM_OBJ)
	ar rcs $@ $^

$(B)/olm/%.cpp.o: $(OLM)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -std=c++11 $(OLM_CPPFLAGS) -c $< -o $@

$(B)/olm/%.c.o: $(OLM)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -w $(OLM_CPPFLAGS) -c $< -o $@

$(B)/app/%.o: src/%
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(OLM_CPPFLAGS) -I$(CJSON) -c $< -o $@

$(B)/app/cJSON.c.o: $(CJSON)/cJSON.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf $(B)

.PHONY: all clean
