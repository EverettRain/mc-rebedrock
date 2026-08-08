#include "render/Frustum.hpp"

#include <glm/mat4x4.hpp>

#include <cassert>

int main() {
    const mc::render::Frustum identity{glm::mat4{1.0F}};

    assert(identity.intersects({{-0.5F, -0.5F, 0.1F}, {0.5F, 0.5F, 0.9F}}));
    assert(identity.intersects({{0.9F, -0.1F, 0.1F}, {1.1F, 0.1F, 0.2F}}));
    assert(!identity.intersects({{2.0F, -0.5F, 0.1F}, {3.0F, 0.5F, 0.9F}}));
    assert(!identity.intersects({{-0.5F, -0.5F, -2.0F}, {0.5F, 0.5F, -0.1F}}));
    assert(!identity.intersects({{-0.5F, -0.5F, 1.1F}, {0.5F, 0.5F, 2.0F}}));
    return 0;
}
