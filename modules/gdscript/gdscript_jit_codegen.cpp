#include "gdscript_jit_codegen.h"
#include "thirdparty/asmjit/asmjit.h"

GDScriptJITCodeGenerator::GDScriptJITCodeGenerator() {
}

uint32_t GDScriptJITCodeGenerator::add_parameter(const StringName &p_name, bool p_is_optional, const GDScriptDataType &p_type) {
	return 0;
}

uint32_t GDScriptJITCodeGenerator::add_local(const StringName &p_name, const GDScriptDataType &p_type) {
	return 0;
}

uint32_t GDScriptJITCodeGenerator::add_local_constant(const StringName &p_name, const Variant &p_constant) {
	return 0;
}

uint32_t GDScriptJITCodeGenerator::add_or_get_constant(const Variant &p_constant) {
	return 0;
}

uint32_t GDScriptJITCodeGenerator::add_or_get_name(const StringName &p_name) {
	return 0;
}

uint32_t GDScriptJITCodeGenerator::add_temporary(const GDScriptDataType &p_type) {
	return 0;
}

void GDScriptJITCodeGenerator::pop_temporary() {
}

void GDScriptJITCodeGenerator::clear_temporaries() {
}
