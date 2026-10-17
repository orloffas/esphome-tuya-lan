# Host test for the Tuya LAN protocol codec.
# Requires mbedtls (macOS: `brew install mbedtls`; Debian: `apt install libmbedtls-dev`).
CXX ?= c++
# TUYA_LAN_HOST_BUILD selects the rand() RNG fallback; on a device ESP_PLATFORM
# is defined instead and esp_random() is used (see tuya_protocol.cpp).
CXXFLAGS ?= -std=c++17 -Wall -Wextra -O1 -g -DTUYA_LAN_HOST_BUILD

# Discover mbedtls headers/libs from Homebrew when present, else rely on system paths.
# Prefer mbedtls@3: ESP-IDF ships mbedtls 3.x, and mbedtls 4.x moved the classic
# AES/GCM API behind private headers. macOS: `brew install mbedtls@3`.
MBEDTLS_PREFIX := $(shell brew --prefix mbedtls@3 2>/dev/null || brew --prefix mbedtls 2>/dev/null)
ifneq ($(MBEDTLS_PREFIX),)
  CXXFLAGS += -I$(MBEDTLS_PREFIX)/include
  LDFLAGS += -L$(MBEDTLS_PREFIX)/lib
endif
LDLIBS += -lmbedcrypto

SRC := ../../components/tuya_lan/tuya_protocol.cpp test_protocol.cpp

.PHONY: test clean fixtures
test: fixtures test_protocol
	./test_protocol fixtures.kv

fixtures: fixtures.kv
fixtures.kv: gen_fixtures.py
	uv run --quiet --with tinytuya python gen_fixtures.py

test_protocol: $(SRC) ../../components/tuya_lan/tuya_protocol.h
	$(CXX) $(CXXFLAGS) $(SRC) $(LDFLAGS) $(LDLIBS) -o $@

clean:
	rm -f test_protocol
