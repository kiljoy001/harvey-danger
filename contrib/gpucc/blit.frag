#version 310 es
precision mediump float;
layout(binding = 0) uniform sampler2D tex;
layout(location = 0) out vec4 colour;
void main()
{
	vec2 uv = gl_FragCoord.xy / vec2(1920.0, 1080.0);
	colour = textureLod(tex, uv, 0.0);
}
