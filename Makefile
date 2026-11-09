EMXX ?= em++
CXX ?= c++
SOURCES = engine/web.cpp engine/catan.cpp engine/agent.cpp engine/graph.cpp
EXPORTS = '["_pig_init","_pig_new","_pig_act","_pig_replay","_pig_bot","_pig_snapshot"]'
WASM_FLAGS = -std=c++17 -O3 -msimd128 -ffp-contract=off -fno-builtin-catan --no-entry -sMODULARIZE=1 -sEXPORT_NAME=createPig -sENVIRONMENT=web,worker,node -sEXPORTED_FUNCTIONS=$(EXPORTS) -sEXPORTED_RUNTIME_METHODS='["UTF8ToString","HEAPU8"]' -sINCOMING_MODULE_JS_API='["mainScriptUrlOrBlob","locateFile","onAbort","print","printErr"]' -sSTACK_SIZE=8388608 -sINITIAL_MEMORY=67108864 -sALLOW_MEMORY_GROWTH=1 -sMAXIMUM_MEMORY=536870912

.PHONY: build test test-wasm serve
build:
	python3 scripts/verify.py
	mkdir -p dist
	cp web/* dist/
	cp model.json dist/
	$(EMXX) $(SOURCES) $(WASM_FLAGS) --embed-file weights -o dist/pig-single.js
	$(EMXX) $(SOURCES) $(WASM_FLAGS) -pthread -sPTHREAD_POOL_SIZE=3 -sPTHREAD_POOL_SIZE_STRICT=2 --embed-file weights -o dist/pig-threaded.js

test:
	python3 scripts/verify.py
	mkdir -p build
	$(CXX) -std=c++17 -O3 -ffp-contract=off -pthread -fno-builtin-catan tests/native.cpp engine/catan.cpp engine/agent.cpp engine/graph.cpp -o build/test
	./build/test

test-wasm:
	mkdir -p build
	$(EMXX) -std=c++17 -O3 -msimd128 -ffp-contract=off -fno-builtin-catan tests/native.cpp engine/catan.cpp engine/agent.cpp engine/graph.cpp --embed-file weights --embed-file tests/golden.txt -sSTACK_SIZE=8388608 -sALLOW_MEMORY_GROWTH=1 -sENVIRONMENT=node -o build/rules-test.js
	node build/rules-test.js

serve:
	python3 serve.py
