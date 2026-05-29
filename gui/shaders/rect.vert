#version 450

layout(push_constant) uniform PushConstants {
	vec4 rect;
	vec4 color;
	vec2 viewport;
} pc;

layout(location = 0) out vec4 v_color;

void main() {
	vec2 corners[6] = vec2[](
		vec2(0.0, 0.0),
		vec2(1.0, 0.0),
		vec2(0.0, 1.0),
		vec2(0.0, 1.0),
		vec2(1.0, 0.0),
		vec2(1.0, 1.0)
	);
	vec2 p = pc.rect.xy + corners[gl_VertexIndex] * pc.rect.zw;
	vec2 ndc = vec2((p.x / pc.viewport.x) * 2.0 - 1.0, (p.y / pc.viewport.y) * 2.0 - 1.0);
	gl_Position = vec4(ndc, 0.0, 1.0);
	v_color = pc.color;
}
