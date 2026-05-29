#version 450

layout(push_constant) uniform PushConstants {
	vec4 rect;
	vec4 uv;
	vec4 color;
	vec2 viewport;
} pc;

layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec4 v_color;

void main() {
	vec2 corners[6] = vec2[](
		vec2(0.0, 0.0),
		vec2(1.0, 0.0),
		vec2(0.0, 1.0),
		vec2(0.0, 1.0),
		vec2(1.0, 0.0),
		vec2(1.0, 1.0)
	);
	vec2 c = corners[gl_VertexIndex];
	vec2 p = pc.rect.xy + c * pc.rect.zw;
	vec2 ndc = vec2((p.x / pc.viewport.x) * 2.0 - 1.0, (p.y / pc.viewport.y) * 2.0 - 1.0);
	gl_Position = vec4(ndc, 0.0, 1.0);
	v_uv = pc.uv.xy + c * pc.uv.zw;
	v_color = pc.color;
}
