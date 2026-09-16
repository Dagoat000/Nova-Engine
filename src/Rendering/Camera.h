// File: src/Rendering/Camera.h
//
// FPS-style camera: position + yaw/pitch instead of a fixed look-at target,
// so Application can drive it directly from WASD + mouse-look input every
// frame. Pitch is clamped just short of +/-90 degrees to avoid the view
// flipping through the up vector (gimbal flip, not full gimbal lock, since
// we never touch roll).
#pragma once

#include <DirectXMath.h>
#include <algorithm>
#include <cmath>

namespace rendering
{
    class Camera
    {
    public:
        void SetPosition(const DirectX::XMFLOAT3& pos) { m_position = pos; }
        const DirectX::XMFLOAT3& GetPosition() const { return m_position; }

        void SetLens(float fovYRadians, float aspectRatio, float nearZ, float farZ)
        {
            m_fovY = fovYRadians;
            m_aspect = aspectRatio;
            m_nearZ = nearZ;
            m_farZ = farZ;
        }
        void SetAspectRatio(float aspectRatio) { m_aspect = aspectRatio; }

        // yawDelta/pitchDelta in radians, typically mouseDeltaPixels * sensitivity.
        void Rotate(float yawDelta, float pitchDelta)
        {
            m_yaw += yawDelta;
            m_pitch = std::clamp(m_pitch + pitchDelta, -kMaxPitch, kMaxPitch);
        }

        // `amount` is signed distance (already scaled by speed * deltaTime).
        void MoveForward(float amount)
        {
            using namespace DirectX;
            XMVECTOR forward = GetForwardVector();
            XMVECTOR pos = XMLoadFloat3(&m_position);
            pos = XMVectorAdd(pos, XMVectorScale(forward, amount));
            XMStoreFloat3(&m_position, pos);
        }

        void MoveRight(float amount)
        {
            using namespace DirectX;
            XMVECTOR forward = GetForwardVector();
            XMVECTOR up = XMVectorSet(0, 1, 0, 0);
            XMVECTOR right = XMVector3Normalize(XMVector3Cross(up, forward));
            XMVECTOR pos = XMLoadFloat3(&m_position);
            pos = XMVectorAdd(pos, XMVectorScale(right, amount));
            XMStoreFloat3(&m_position, pos);
        }

        void MoveUp(float amount)
        {
            m_position.y += amount;
        }

        DirectX::XMMATRIX GetViewMatrix() const
        {
            using namespace DirectX;
            XMVECTOR eye = XMLoadFloat3(&m_position);
            XMVECTOR forward = GetForwardVector();
            XMVECTOR up = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
            return XMMatrixLookToLH(eye, forward, up);
        }

        DirectX::XMMATRIX GetProjectionMatrix() const
        {
            return DirectX::XMMatrixPerspectiveFovLH(m_fovY, m_aspect, m_nearZ, m_farZ);
        }

    private:
        DirectX::XMVECTOR GetForwardVector() const
        {
            using namespace DirectX;
            // Standard yaw/pitch -> direction vector. Yaw rotates around Y,
            // pitch tilts up/down after that - the order that gives you the
            // familiar "look around" feel instead of a tumbling camera.
            float cosPitch = cosf(m_pitch);
            return XMVectorSet(
                cosPitch * sinf(m_yaw),
                sinf(m_pitch),
                cosPitch * cosf(m_yaw),
                0.0f);
        }

        DirectX::XMFLOAT3 m_position{ 0.0f, 1.5f, -4.0f };
        float m_yaw = 0.0f;   // radians, 0 = looking down +Z
        float m_pitch = 0.0f; // radians, positive = looking up

        float m_fovY = DirectX::XM_PIDIV4;
        float m_aspect = 16.0f / 9.0f;
        float m_nearZ = 0.1f;
        float m_farZ = 100.0f;

        static constexpr float kMaxPitch = DirectX::XM_PIDIV2 - 0.01f;
    };
}
