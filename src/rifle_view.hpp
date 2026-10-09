#pragma once
#include "viewmodel.hpp"

// The FAL in first person: the bolt cycles in the ejection port with each
// shot and throws the case out to the right, locks back on an empty
// magazine, and the reload swaps magazines (rocked out about the front hook,
// dropped, the new one hooked in and rocked home) and, from empty, pulls the
// charging handle to close the bolt.
class RifleView : public WeaponView {
public:
    explicit RifleView(const WeaponModels& models) : WeaponView(models) {}

    void Update(float dt, const ViewModelMotion& motion, const Weapon& weapon, const Camera3D* worldCamera = nullptr) override;
    void DryFire() override { sinceDry = 0.0f; }
    void Draw(const Weapon& weapon) const override;
    const FalPose& Mechanism() const { return mech; }

    // The reload's script, in seconds (the weapon's reload times match these)
    static constexpr float SWAP_END = 2.0f, TACTICAL_END = 2.6f, EMPTY_END = 3.4f;

private:
    ArmsClip Clip() const override;
    float ArmsFrame(const Weapon& weapon) const override;
    ReloadPose FallbackReloadPose() const override { return reloadPose; }
    float ReloadSeconds(const Weapon& weapon) const; // how far into the reload's script
    void OnShot(const Weapon& weapon);
    void EjectCase(const Weapon& weapon);
    void AnimateReload(float t, const Weapon& weapon);
    Vector3 LeftPalmInGun(const Weapon& weapon) const; // the support hand's palm, in gun space

    FalPose mech;
    int lastAmmo = -1;
    float sinceDry = 10.0f;
    bool casePending = false;   // the fired case comes out once the bolt is back
    bool wasReloading = false;
    float reloadTime = 0.0f;
    bool magDropped = false, newMag = false;
    ReloadPose reloadPose{};
};
