# Twelve-minute speaker script / 十二分钟讲稿

English slides and script, with Chinese rehearsal cues. Main 12 slides planned: 12:00, plus two untimed Q&A appendices. The actual duration needs a spoken rehearsal.

Open `StarTraveler-denoiser-presentation-r2.pptx`. Main figures use the fixed-exposure settling dataset; appendices use the separate 27-run Quality motion study. Use the accepted fixed-exposure flight backup as illustration.

## 1. Denoising after motion (0:00–0:45)

How much ray tracing does a rasterizer need for convincing real-time lighting? Our project approaches that question through a hybrid renderer. My part studies what happens to its noisy output when we reuse samples over time, filter them spatially and reconstruct the final image. Today I will focus on the completed production-resolution comparison after motion stops. I will keep two outcomes separate: how early a method reaches its own stable error range, and how large that eventual error is. They lead to different conclusions.

中文提示：开场只提出研究问题，不先宣称 RR 胜出。

## 2. The reconstruction pipelines (0:45–1:45)

The hybrid renderer starts from rasterized visibility and uses ray queries for paths. This lets us study reconstruction without introducing a new rendering method. In the first pipeline, Full A-SVGF reprojects history with motion vectors and applies spatial filtering, followed by ordinary DLSS K. The other pipeline uses DLSS Ray Reconstruction. This is an end-to-end comparison between the implemented pipelines. It is not an isolated cost measurement of one filter. Earlier native-resolution ablations established why filtering is useful, especially in the Anvil and star views, but the mirror also showed that more filtering does not improve every scene uniformly.

中文提示：Full/Half 涉及光线预算，不能把差值全算成降噪器开销。

## 3. Controlled comparison (1:45–2:45)

Both methods reconstruct the same output size from the same internal resolution at one sample per pixel. We use three random seeds and retain temporal history. The temporal input is fixed at one sixtieth of a second, so a slower PNG export cannot change that input. Ordinary DLSS explicitly selects model K with fixed exposure one. RR retains its unity input, and both use manual post-processing exposure one. This separates DLSS normalization from tone mapping. All rendering was on the same RTX 3070 laptop connected to AC power. The fixed time step is a simulation input. It does not mean the renderer achieved sixty frames per second.

中文提示：强调 16.667 ms 是固定输入，不是实测 GPU 耗时。

## 4. Three fixed motion paths (2:45–3:40)

The three paths stress different content. The cockpit turns toward the Anvil, the mirror and glass view translates, and the star path turns rapidly across bright structures. These are fixed per-frame updates, not a manual flight and not speeds in seconds. Each moving prefix is followed by five hundred and twelve stationary frames. For each shot the horizon is shared across both methods and all seeds. The predeclared plan allowed extending the horizon if any sequence failed the criterion. All eighteen physical sequences passed at the initial horizon, so we did not extend a convenient subset or change the threshold after seeing the result.

中文提示：三个正式镜头不等于所有场景；现场飞行只是示意。

## 5. Consistent history and exposure (3:40–4:40)

Before measuring settling, we had to establish that the experiment itself was consistent. Extending a stationary tail had unexpectedly changed its moving prefix. We corrected the initial previous transforms, removing false object motion on a stationary first frame. That fix alone did not resolve the auto-exposure controls. Providing explicit fixed exposure to DLSS K gave us a controlled option for which the eighty moving PNGs match exactly between short and long sequences. Endpoint, time, exposure and reference compatibility checks also pass. This demonstrates a working controlled configuration. It does not uniquely prove an undocumented internal cause inside the DLSS SDK.

中文提示：修复证据与 SDK 内部成因推断分开说。

## 6. Settling and terminal error (4:40–5:45)

For each error curve, the final sixty-four-frame median defines its terminal estimate. The two halves of that window must have at most three percent drift. Entry is the first stationary frame after which the entire observed remainder stays within one hundred and ten percent of that terminal estimate, with at least sixteen observations. These rules were kept unchanged. Crucially, each method has its own terminal estimate. A method can enter a higher-error band earlier. Therefore entry measures adaptation toward its own eventual state, not time to a common absolute quality target. We compare original sRGB images using LDR-FLIP against two independent eight-thousand-one-hundred-and-ninety-two-sample references.

中文提示：这是整场最重要的定义，先说明再展示结果。

## 7. RR enters its own band earlier (5:45–7:00)

Against the primary reference, Anvil entry is one hundred and sixteen frames for Full plus DLSS K and forty-seven for RR. Mirror is two hundred and eighty-six versus one, while star is one hundred and seventy-two versus one hundred and thirty-one. RR enters its own terminal band earlier on all three paths. The mirror value needs care: RR already has eighty motion updates of accumulated history when the view stops. Entry at frame one or two means it was already near its eventual band. It does not mean one-frame end-to-end latency. These are frame indices within the finite observation, not wall-clock timing or an FPS benchmark.

中文提示：镜面 1–2 帧不是输入延迟，也不是从零历史重建只需一帧。

## 8. Terminal error favors different choices (7:00–8:15)

The terminal-error comparison changes the interpretation. Full plus DLSS K has lower observed FLIP on Anvil, about zero point zero three seven versus zero point zero four two, and on the star, about zero point zero three two versus zero point zero three six. The mirror values are very close, around zero point zero one zero five. The second reference preserves these descriptive orderings. So earlier entry does not establish better final quality. With just three seeds and noisy references, I describe the measured differences without calling them statistically significant. The result is a scene-dependent tradeoff rather than one pipeline winning every measure.

中文提示：Anvil 和星体 Full+K 最终误差更低；镜面接近。不做显著性结论。

## 9. Seed and reference sensitivity (8:15–9:10)

The medians are not the whole result. Changing the reference moves one Anvil RR run from forty-seven to seventy-nine frames, although its group median stays forty-seven. Star RR varies from sixty-seven to one hundred and thirty-one across seeds. Mirror RR ranges from one to two. Both references and every seed remain in the report and figures. Comparing the same physical capture against a second reference does not create a new independent render. Reference-pair FLIP also shows that the references retain noise. Those values are sensitivity information, not statistical confidence bounds and not a rule that automatically makes smaller differences meaningless.

中文提示：双参考是敏感性分析，不能把样本量说成 6 个种子。

## 10. Final stationary frames (9:10–10:05)

These are the final stationary frames using the same illustrative seed for both methods. Rows show the three shots, and columns show Full plus DLSS K, RR and the reference. The images confirm the matching view and provide a visual companion to the curves. They are resized thumbnails for presentation, not the inputs used for scoring. Every reported statistic comes from original equal-sized seven-twenty-p images and all three seeds. I would use the full-size saved images for detailed questions about a region, rather than implying that a small projected contact sheet proves a subtle perceptual difference.

中文提示：缩略图只帮助认镜头，精细画质问题打开原始图。

## 11. What these results establish (10:05–11:05)

All eighteen settling captures and thirty-six reference outcomes pass the unchanged finite-capture rule. That study observes one laptop and three paths. A separate twenty-seven-run Quality comparison now measures moving-path GPU cost and the final moving-frame error under fixed temporal and exposure inputs. Its results are in the appendix and report. The settling dump sequences themselves are not timing evidence. We retain the historical table as historical and preserve the known Performance-M color-test failure. The results do not establish significance or universal superiority.

中文提示：不把旧 GPU 耗时移植到新配置，也不说全部 GPU 测试全绿。

## 12. Reconstruction is a scene-dependent tradeoff (11:05–12:00)

The conclusion is that reconstruction should be evaluated with both adaptation and eventual error. On these paths RR reaches its own band earlier, while Full plus DLSS K has lower observed terminal FLIP in the Anvil and star views. Mirror terminal values are close. The fixed exposure and motion controls matter because a comparison is only meaningful if extending the capture does not change its earlier history. Next, the project still needs unified shared-tool evaluation and manual operation, projection and spoken timing checks on the presentation setup. For the presentation, I will show saved evidence if live rendering fails and use the accepted fixed-exposure flight backup as illustration. Thank you, and I am happy to discuss the protocol or individual curves.

中文提示：收尾回到研究结论。第六步才更新真实窗口彩排与备份视频。

## Appendix 13: Appendix: moving-path GPU cost (untimed Q&A)

GPU timestamp medians over each moving path. This is a separate timed capture, not the stationary PNG dump. All three methods use the same Quality input/output sizes. See the report for seed ranges and scope.

中文提示：补充答辩图，不增加主讲十二分钟；这是独立的运动计时实验。

## Appendix 14: Appendix: error at the end of motion (untimed Q&A)

Final moving-frame LDR-FLIP for the same timed runs as the preceding appendix slide. The second reference and all per-seed outcomes remain in the report. Do not substitute these values for the post-stop terminal-error chart.

中文提示：运动结束误差与静止后最终误差不是同一指标；双参考和范围见报告。

## Questions to prepare / 答辩提示

- **Why three seeds?** This is a descriptive project protocol, not a universal statistical requirement. We show ranges and make no significance claim.
- **Does RR win?** Earlier entry into its own band on these paths. Full + K has lower observed terminal FLIP on Anvil and star. Mirror values are close.
- **Is mirror one-frame latency?** No. It includes 80 prior motion updates of history.
- **How fast is the new configuration?** See the separate fixed-input 27-run moving-path study and appendix. Settling dump captures themselves are not timing evidence.
- **Did all GPU tests pass?** No such claim. Three focused regressions pass. The full suite was not rerun, and the deferred M issue remains.
- **Why two references?** To expose reference sensitivity. They do not double the independent render-seed sample size.
- **If the live demo fails?** Show these saved charts and images. Use the accepted fixed-exposure flight backup. Manual operation, projection and spoken timing still need a person on the presentation setup.

AI assistance disclosure and artifact links are in [the report](../final-denoiser-report.md).
