.PHONY: android-arm64 host-smoke plugin-device-test smoke clean

android-arm64:
	./scripts/build-android-arm64.sh

host-smoke:
	./scripts/smoke-host.sh

smoke: host-smoke android-arm64

clean:
	rm -rf out

plugin-device-test:
	plugins/hello/device-test.sh
