# Compile the actual receiver control flow against GLM and a counting sampler.
# Only adapt GLSL swizzle syntax and float literals; do not rewrite its maths,
# conditions, returns or texture calls. Unsupported syntax fails compilation.
# Also callable with cmake -DSHADER_SOURCE=... -DCPP_OUTPUT=... -P for sabotage.
file(READ "${SHADER_SOURCE}" receiver)
string(REPLACE "lightPosition.xyz" "vec3(lightPosition)" receiver "${receiver}")
string(REPLACE "projected.xy" "vec2(projected)" receiver "${receiver}")
string(REPLACE "shadowUv.xy" "vec2(shadowUv)" receiver "${receiver}")
string(REGEX REPLACE "([0-9]+\\.[0-9]+)([Ff]?)" "\\1F" receiver "${receiver}")
file(WRITE "${CPP_OUTPUT}" "${receiver}")
