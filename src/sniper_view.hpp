#pragma once
#include "viewmodel.hpp"

// The L42A1 in first person. After each shot the right hand leaves the wrist for the
// bolt: turns the handle up, draws it back (the case flies out to the right), pushes it
// home on a fresh round and turns it down. The reload opens the bolt, changes the
// magazine and closes the bolt again. Aimed all the way, the telescope takes over the
// whole view (drawn by the HUD), so the gun itself isn't drawn.
class SniperView : public WeaponView {
public:
    explicit SniperView(const WeaponModels& models) : WeaponView(models) {}

    void Update(float dt, const ViewModelMotion& motion, const Weapon& weapon, const Camera3D* worldCamera = nullptr) override;
    void DryFire() override { sinceDry = 0.0f; }
    void Draw(const Weapon& weapon) const override;
    const SniperPose& Mechanism() const { return mech; }

    // The scripts, in seconds (the weapon's times match: L42A1 in weapon.cpp)
    static constexpr float CYCLE_END = 1.15f, RELOAD_END = 3.4f;
    static constexpr float CYCLE_ARMS_FROM = 0.26f; // the right hand stays on the wrist until then

private:
    ArmsClip Clip() const override;
    float ArmsFrame(const Weapon& weapon) const override;
    Vector3 ArmsGrip(const Weapon& weapon) const override;
    bool HandDriven(const Weapon&) const override { return false; } // the hands work it; the keys move the gun
    // The arms were authored reaching for the gun where the keys put it: they stay with the body
    Matrix ArmsAnchor(const Weapon& weapon) const override { return BaseGunToCamera(weapon, false); }
    ReloadPose FallbackReloadPose() const override { return reloadPose; }
    bool WorkingBolt(const Weapon& weapon) const;  // the arms are in the bolt or reload frames
    void OnShot(const Weapon& weapon);
    void Eject(const Weapon& weapon, bool live);
    void AnimateCycle(float prev, float t, const Weapon& weapon);
    void AnimateReload(float t, const Weapon& weapon);
    Vector3 LeftPalmInGun(const Weapon& weapon) const;

    SniperPose mech;
    int lastAmmo = -1;
    float sinceDry = 10.0f;
    float cycleTime = -1.0f;     // seconds into working the bolt (-1: not)
    bool wasReloading = false;
    float reloadTime = 0.0f;
    bool roundInChamber = false; // a live round was chambered when the reload began
    bool magDropped = false;
    ReloadPose reloadPose{};
};
