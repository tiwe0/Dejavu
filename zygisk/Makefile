.PHONY: test build build-template build-lsplant package deploy-agent clean

test:
	./scripts/test-host.sh

build:
	./scripts/build-lsplant-android-arm64.sh

build-template:
	./scripts/build-android-arm64.sh

build-lsplant:
	./scripts/build-lsplant-android-arm64.sh

package: test build
	./scripts/package.sh

deploy-agent: build
	./scripts/deploy-agent.sh

clean:
	rm -rf out dist
