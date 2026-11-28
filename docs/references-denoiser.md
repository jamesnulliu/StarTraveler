# Denoiser study references

Primary sources checked on 27 November 2026. These cover this local study;
the team's original proposal bibliography was not available for reconciliation.

1. Christoph Schied et al. (2017). *Spatiotemporal Variance-Guided Filtering:
   Real-Time Reconstruction for Path-Traced Global Illumination*. HPG.
   [Author/publisher research page](https://research.nvidia.com/labs/rtr/publication/schied2017spatiotemporal/),
   [DOI](https://doi.org/10.1145/3105762.3105770).
   Background for combining temporal sample reuse with variance-guided spatial
   filtering. The paper's hardware numbers are not this project's measurements.

2. Christoph Schied, Christoph Peters and Carsten Dachsbacher (2018).
   *Gradient Estimation for Real-Time Adaptive Temporal Filtering*.
   Proceedings of the ACM on Computer Graphics and Interactive Techniques 1(2),
   Article 24. [Author copy](https://momentsingraphics.de/Media/HPG2018/Schied2018-AdaptiveTemporalFiltering.pdf),
   [DOI](https://doi.org/10.1145/3233301).
   Background for adaptive temporal-history rejection. The project's A-SVGF
   label does not establish exact reproduction of every detail of that paper.

3. Pontus Andersson et al. (2020). *FLIP: A Difference Evaluator for Alternating
   Images*. [Paper hosted by NVIDIA](https://research.nvidia.com/sites/default/files/node/3260/FLIP_Paper.pdf).
   Metric background. The study uses the LDR evaluation path on equal-sized
   sRGB images and retains noisy-reference sensitivity.

4. NVIDIA Research. *FLIP implementation*.
   [Official NVlabs repository](https://github.com/NVlabs/flip).
   Software citation: `flip-evaluator` version 1.7 in the frozen environment,
   LDR-FLIP at 67 PPD. This project does not use HDR-FLIP in the reported study.

5. Zander Majercik et al. (2019). *Dynamic Diffuse Global Illumination with
   Ray-Traced Irradiance Fields*. JCGT 8(2), 1–30.
   [Journal page](https://jcgt.org/published/0008/02/01/),
   [paper](https://jcgt.org/published/0008/02/01/paper-lowres.pdf).
   Background for the renderer's irradiance-probe rung, rather than evidence
   that its quality/cost matches the paper's results.

6. NVIDIA. *DLSS SDK 310.9.1*.
   [Pinned release](https://github.com/NVIDIA/DLSS/releases/tag/v310.9.1).
   Software citation for the SDK used by the renderer. The experiment explicitly
   requests ordinary DLSS model K and separately uses RR. Pinning the SDK is
   distinct from claiming that all vendor-internal behavior is understood.

For measured results, cite the project's report, protocol, exact data CSV and
verification artifact alongside these background references. Experimental
numbers in this repository come from the recorded RTX 3070 Laptop captures,
not from results quoted in the papers. See [AI assistance](ai-assistance.md)
for the accompanying code/analysis disclosure.
