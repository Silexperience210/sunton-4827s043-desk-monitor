# Simulateur PC (headless) — compile l'UI et le cœur de VelohNav Ops avec LVGL 8.3
# make LVGL_DIR=/chemin/lvgl AJSON_DIR=/chemin/ArduinoJson/src
LVGL_DIR  ?= ../.deps/lvgl
AJSON_DIR ?= ../.deps/ArduinoJson/src
BUILD     ?= build

CC  ?= gcc
CXX ?= g++
INC  = -I../include -I$(LVGL_DIR)/.. -I$(LVGL_DIR) -I$(AJSON_DIR) -I../src
DEFS = -DLV_CONF_INCLUDE_SIMPLE -DVN_SIM=1
CFLAGS   = -O2 -g -Wall -Wno-unused-function -MMD -MP $(INC) $(DEFS)
# -MMD -MP : chaque objet dépend aussi des en-têtes qu'il inclut (sinon un objet
# compilé avec un ancien model.h survit à un changement de structure et plante)
CXXFLAGS = -MMD -MP -O2 -g -std=gnu++17 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers $(INC) $(DEFS)

LVGL_SRC := $(shell find $(LVGL_DIR)/src -name '*.c')
APP_C    := $(wildcard ../src/ui/fonts/*.c) $(wildcard ../src/ui/icons/*.c)
APP_CXX  := $(wildcard ../src/core/*.cpp) $(wildcard ../src/ui/*.cpp) $(wildcard ../src/ui/widgets/*.cpp) \
            ../src/hal/touch_cal.cpp platform_sim.cpp net_sim.cpp sim_main.cpp

LVGL_OBJ := $(patsubst $(LVGL_DIR)/%.c,$(BUILD)/lvgl/%.o,$(LVGL_SRC))
APP_OBJ  := $(patsubst ../%.c,$(BUILD)/app/%.o,$(APP_C)) \
            $(patsubst ../%.cpp,$(BUILD)/app/%.o,$(filter ../%,$(APP_CXX))) \
            $(patsubst %.cpp,$(BUILD)/sim/%.o,$(filter-out ../%,$(APP_CXX)))

sim: $(LVGL_OBJ) $(APP_OBJ)
	$(CXX) -o $@ $^ -lm

$(BUILD)/lvgl/%.o: $(LVGL_DIR)/%.c
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS) -w -c $< -o $@
$(BUILD)/app/%.o: ../%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/app/%.o: ../%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@
$(BUILD)/sim/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

TEST_CXX := $(wildcard ../src/core/*.cpp) ../src/hal/touch_cal.cpp platform_sim.cpp test_core.cpp
TEST_OBJ := $(patsubst ../%.cpp,$(BUILD)/app/%.o,$(filter ../%,$(TEST_CXX))) \
            $(patsubst %.cpp,$(BUILD)/sim/%.o,$(filter-out ../%,$(TEST_CXX)))
test_core: $(TEST_OBJ)
	$(CXX) -o $@ $^ -lm

# Banc de mesure de la prévision (docs/mesures-prevision.md) : même cœur, aucune UI
BENCH_CXX := $(wildcard ../src/core/*.cpp) platform_sim.cpp bench_prevision.cpp
BENCH_OBJ := $(patsubst ../%.cpp,$(BUILD)/app/%.o,$(filter ../%,$(BENCH_CXX))) \
             $(patsubst %.cpp,$(BUILD)/sim/%.o,$(filter-out ../%,$(BENCH_CXX)))
bench_prevision: $(BENCH_OBJ)
	$(CXX) -o $@ $^ -lm

# Émetteur simulé de la commande série « dump » (tools/test_carte_dump.py) : même cœur, aucune UI
DUMP_CXX := $(wildcard ../src/core/*.cpp) platform_sim.cpp dump_sim.cpp
DUMP_OBJ := $(patsubst ../%.cpp,$(BUILD)/app/%.o,$(filter ../%,$(DUMP_CXX))) \
            $(patsubst %.cpp,$(BUILD)/sim/%.o,$(filter-out ../%,$(DUMP_CXX)))
dump_sim: $(DUMP_OBJ)
	$(CXX) -o $@ $^ -lm

# dépendances d'en-têtes (-MMD) : un .h modifié recompile ce qui l'inclut
-include $(APP_OBJ:.o=.d) $(TEST_OBJ:.o=.d) $(BENCH_OBJ:.o=.d) $(DUMP_OBJ:.o=.d)

clean:
	rm -rf $(BUILD) sim test_core bench_prevision dump_sim

.PHONY: clean
