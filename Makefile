.PHONY: android-arm64 host-smoke plugin-device-test stream-receiver smoke clean

android-arm64:
	./scripts/build-android-arm64.sh

host-smoke:
	./scripts/smoke-host.sh

smoke: host-smoke android-arm64

clean:
	rm -rf out

stream-receiver:
	mkdir -p out/host-stream
	$(CC) -O2 -I zygisk/third_party/kcp -c zygisk/third_party/kcp/ikcp.c -o out/host-stream/ikcp.o
	$(CXX) -std=c++17 -O2 -Wall -Wextra -I zygisk/third_party/kcp tools/stream_receiver.cpp out/host-stream/ikcp.o -o out/host-stream/stream_receiver

plugin-device-test:
	plugins/hello/device-test.sh
