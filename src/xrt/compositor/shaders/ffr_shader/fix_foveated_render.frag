#version 450
//#extension GL_ARB_separate_shader_objects : enable

layout(binding = 0) uniform sampler2D texSampler;

layout(binding = 1, std140) uniform UniformBufferObject {
     uvec2 targetResolution;
     uvec2 optimizedResolution;
     vec2  eyeSizeRatio;
     vec2  centerSize;
     vec2  centerShift;
     vec2  edgeRatio;
} ubo;

vec2 TextureToEyeUV(vec2 textureUV, bool isRightEye) {
	// flip distortion horizontally for right eye
	// left: x * 2; right: (1 - x) * 2
	return vec2((textureUV.x + float(isRightEye) * (1.0 - 2.0 * textureUV.x)) * 2.0, textureUV.y);
}

vec2 EyeToTextureUV(vec2 eyeUV, bool isRightEye) {
	// saturate is used to avoid color bleeding between the two sides of the texture or with the black border when filtering
	//float2 clampedUV = saturate(eyeUV);
	// left: x / 2; right 1 - (x / 2)
	//return float2(clampedUV.x / 2. + float(isRightEye) * (1. - clampedUV.x), clampedUV.y);
	return vec2(eyeUV.x / 2.0 + float(isRightEye) * (1.0 - eyeUV.x), eyeUV.y);
}

layout(location = 0) in vec2 in_uv;

layout(location = 0) out vec4 outColor;

void main() {
	bool isRightEye = in_uv.x > 0.5;
    vec2 eyeUV = TextureToEyeUV(in_uv, isRightEye);
   
    vec2 alignedUV = eyeUV / ubo.eyeSizeRatio;

	vec2 c0 = (1.0 - ubo.centerSize) / 2.0;
	vec2 c1 = (ubo.edgeRatio - 1.0) * c0 * (ubo.centerShift + 1.0) / ubo.edgeRatio;
	vec2 c2 = (ubo.edgeRatio - 1.0) * ubo.centerSize + 1.0;

    vec2 loBound = c0 * (ubo.centerShift + 1.0) / c2;
	vec2 hiBound = c0 * (ubo.centerShift - 1.0) / c2 + 1.0;
	vec2 underBound = vec2(alignedUV.x < loBound.x, alignedUV.y < loBound.y);
	vec2 inBound = vec2(loBound.x < alignedUV.x && alignedUV.x < hiBound.x, loBound.y < alignedUV.y && alignedUV.y < hiBound.y);
	vec2 overBound = vec2(alignedUV.x > hiBound.x, alignedUV.y > hiBound.y);

	vec2 d1 = alignedUV * c2 / ubo.edgeRatio + c1;
	vec2 d2 = alignedUV * c2;
	vec2 d3 = (alignedUV - 1.0) * c2 + 1.0;
	vec2 g1 = alignedUV / loBound;
	vec2 g2 = (1.0 - alignedUV) / (1.0 - hiBound);

	vec2 center = d1;
	vec2 leftEdge = g1 * d1 + (1.0 - g1) * d2;
	vec2 rightEdge = g2 * d1 + (1.0 - g2) * d3;

	vec2 compressedUV = underBound * leftEdge + inBound * center + overBound * rightEdge;

    outColor = texture(texSampler, EyeToTextureUV(compressedUV, isRightEye));
	//outColor = vec4(in_uv.x, 0.0, in_uv.y, 1.0);
}
