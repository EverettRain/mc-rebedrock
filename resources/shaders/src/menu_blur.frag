#version 450

// UI-5：整帧盒式模糊的一趟，逐行对应 26.1 的
// `assets/minecraft/shaders/post/box_blur.fsh`。
//
// 它**依赖双线性采样**：以 2 为步长在像素之间采样，一次拿到相邻两像素的平均，
// 采样次数因此减半；末尾再补一次半权重的边缘采样，因为要平均的像素总数
// （actualRadius * 2 + 1）永远是奇数。把采样器换成最近邻，同样的循环会漏掉一半
// 像素，画面变成竖条纹而不是模糊——所以 MenuBlur 的采样器必须是 LINEAR。
//
// 半径由 26.1 的 `Options.menuBackgroundBlurriness` 给（整数档 0..10，默认 5）：
// blur.json 把 `Radius` 这个 uniform 写成 0，于是 box_blur.fsh 里 `Radius >= 0.5`
// 不成立，取的是全局 `MenuBlurRadius`，也就是选项值本身。
layout(binding = 0) uniform sampler2D inSampler;

layout(push_constant) uniform BlurPush {
    // xy = BlurDir（(1,0) 是横向那趟，(0,1) 是纵向那趟），z = 半径（像素），w 保留
    vec4 config;
} blur;

layout(location = 0) in vec2 texCoord;
layout(location = 0) out vec4 outColor;

void main() {
    vec2 oneTexel = 1.0 / vec2(textureSize(inSampler, 0));
    vec2 sampleStep = oneTexel * blur.config.xy;

    float actualRadius = blur.config.z;
    vec4 blurred = vec4(0.0);
    for (float a = -actualRadius + 0.5; a <= actualRadius; a += 2.0) {
        blurred += texture(inSampler, texCoord + sampleStep * a);
    }
    blurred += texture(inSampler, texCoord + sampleStep * actualRadius) / 2.0;
    outColor = blurred / (actualRadius + 0.5);
}
