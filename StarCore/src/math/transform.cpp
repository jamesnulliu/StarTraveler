module starcore.math;

import std;
import glm;

namespace starcore
{

auto composeTransform(Vec3 translation, Quat rotation, Vec3 scale) -> Mat4
{
    Mat4 m = glm::mat4_cast(glm::normalize(rotation));
    m[0] *= scale.x;
    m[1] *= scale.y;
    m[2] *= scale.z;
    m[3] = Vec4(translation, 1.0F);
    return m;
}

auto decomposeTransform(const Mat4& matrix) -> DecomposedTransform
{
    DecomposedTransform result;
    result.translation = Vec3(matrix[3]);

    Vec3 columns[3] = {Vec3(matrix[0]), Vec3(matrix[1]), Vec3(matrix[2])};
    result.scale = {glm::length(columns[0]), glm::length(columns[1]),
                    glm::length(columns[2])};
    // A negative determinant means a mirror; fold it into the X scale.
    if (glm::dot(columns[0], glm::cross(columns[1], columns[2])) < 0.0F) {
        result.scale.x = -result.scale.x;
    }
    for (int i = 0; i < 3; ++i) {
        if (result.scale[i] != 0.0F) {
            columns[i] /= result.scale[i];
        }
    }
    result.rotation =
        glm::normalize(glm::quat_cast(Mat3(columns[0], columns[1], columns[2])));
    return result;
}

auto rotationBetween(Vec3 from, Vec3 to) -> Quat
{
    const float fromLength = glm::length(from);
    const float toLength = glm::length(to);
    if (fromLength == 0.0F || toLength == 0.0F) {
        return {1.0F, 0.0F, 0.0F, 0.0F};
    }
    // rotation() picks an arbitrary orthogonal axis for opposite vectors. It is an experimental
    // extension: glm's module exports those in glm::gtx (see CMakeLists.txt).
    return glm::gtx::rotation(from / fromLength, to / toLength);
}

}  // namespace starcore
