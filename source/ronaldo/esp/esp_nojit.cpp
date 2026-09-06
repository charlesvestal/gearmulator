/* Placeholder translation unit for the JE_ESP_NO_JIT build: the ESP is
 * header-only without the emitters, and a static library needs one object. */
#include "esp.hpp"

extern "C" void esp_nojit_translation_unit() {}
