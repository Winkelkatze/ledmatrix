
LEDMATRIX_MOD_DIR := $(USERMOD_DIR)

# Add all C files to SRC_USERMOD.
SRC_USERMOD += $(LEDMATRIX_MOD_DIR)/ledmatrix.c
SRC_USERMOD += $(LEDMATRIX_MOD_DIR)/esp_i2s_parallel/src/i2s_parallel.c

# We can add our module folder to include paths if needed
# This is not actually needed in this example.
CFLAGS_USERMOD += -I$(LEDMATRIX_MOD_DIR)/esp_i2s_parallel/include

