#include "render/PerspectiveCamera.hpp"

#include <cassert>
#include <cmath>
#include <stdexcept>

int main() {
    const mc::render::PerspectiveCamera camera{
        {2.7F, 2.2F, 2.7F}, {0.0F, 0.0F, 0.0F}, 45.0F};
    const auto projection = camera.projectionMatrix(16.0F / 9.0F);
    const auto view = camera.viewMatrix();

    assert(std::isfinite(projection[0][0]));
    assert(projection[1][1] < 0.0F); // Vulkan framebuffer Y direction correction.
    assert(std::isfinite(view[3][2]));

    bool rejectedInvalidAspect = false;
    try {
        static_cast<void>(camera.projectionMatrix(0.0F));
    } catch (const std::invalid_argument&) {
        rejectedInvalidAspect = true;
    }
    assert(rejectedInvalidAspect);
    bool rejectedInvalidFarPlane = false;
    try {
        static_cast<void>(camera.projectionMatrix(1.0F, 0.1F));
    } catch (const std::invalid_argument&) {
        rejectedInvalidFarPlane = true;
    }
    assert(rejectedInvalidFarPlane);
    return 0;
}
