// Automatic exposure, as render/exposure.hpp (the same constants and metering).

const uint kExposureBins = 96u;
const float kExposureMinLog2 = -14.0;
const float kExposureMaxLog2 = 18.0;
const float kVeilingGlare = 0.05;
const float kExposureLowPercentile = 0.5;
const float kExposureHighPercentile = 0.96;
const float kExposureKey = 0.18;
const float kMinExposure = 0.05;
const float kMaxExposure = 12.0;
const float kDarkeningRate = 3.0;
const float kBrighteningRate = 1.2;

uint exposureBin(float luminance)
{
    if (!(luminance > exp2(kExposureMinLog2))) {
        return 0u;
    }
    float t = (log2(luminance) - kExposureMinLog2) / (kExposureMaxLog2 - kExposureMinLog2);
    return uint(clamp(t * float(kExposureBins - 1u) + 1.0, 1.0, float(kExposureBins - 1u)));
}
