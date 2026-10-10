#include "player.hpp"
#include "raymath.h"

void Player::Init(Vector3 feetPosition, float facingYaw) {
    feet = feetPosition;
    yaw = facingYaw;
    pitch = 0.0f;
    velocityY = 0.0f;
    grounded = false;
    crouching = false;
    stance = Stance::Still;
    leanAmount = 0.0f;
    velocity = { 0, 0, 0 };
    airTickTimer = 0.0f;
    health = maxHealth;
    currentEyeHeight = eyeHeight;
    stepSmooth = 0.0f;
    camera.position = { feet.x, feet.y + eyeHeight, feet.z };
    camera.up = { 0.0f, 1.0f, 0.0f };
    camera.fovy = baseFovy;
    camera.projection = CAMERA_PERSPECTIVE;
    camera.target = Vector3Add(camera.position, GetLookDirection());
}

Vector3 Player::GetLookDirection() const {
    return {
        sinf(yaw) * cosf(pitch),
        sinf(pitch),
        cosf(yaw) * cosf(pitch),
    };
}

void Player::SetZoom(float amount) {
    camera.fovy = Lerp(baseFovy, zoomFovy, Clamp(amount, 0.0f, 1.0f));
}

PlayerInput Player::ReadInput() {
    PlayerInput in;
    in.look = GetMouseDelta();
    in.forward = IsKeyDown(KEY_W);
    in.back = IsKeyDown(KEY_S);
    in.left = IsKeyDown(KEY_A);
    in.right = IsKeyDown(KEY_D);
    in.sprint = IsKeyDown(KEY_LEFT_SHIFT);
    in.crouch = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_C);
    in.leanLeft = IsKeyDown(KEY_Q);
    in.leanRight = IsKeyDown(KEY_E);
    in.jump = IsKeyPressed(KEY_SPACE);
    return in;
}

void Player::LookAngles(Vector2 delta, float& yawOut, float& pitchOut) const {
    float sensitivity = mouseSensitivity * camera.fovy / baseFovy; // slower when zoomed
    yawOut = yaw - delta.x * sensitivity;
    pitchOut = Clamp(pitch - delta.y * sensitivity, -1.5f, 1.5f);
}

PlayerState Player::GetState() const {
    PlayerState s;
    s.feet = feet;
    s.yaw = yaw;
    s.pitch = pitch;
    s.velocity = velocity;
    s.velocityY = velocityY;
    s.airTickTimer = airTickTimer;
    s.grounded = grounded;
    s.crouching = crouching;
    s.leanAmount = leanAmount;
    s.eyeHeight = currentEyeHeight;
    s.health = health;
    s.stance = stance;
    return s;
}

void Player::SetState(const PlayerState& s) {
    feet = s.feet;
    yaw = s.yaw;
    pitch = s.pitch;
    velocity = s.velocity;
    velocityY = s.velocityY;
    airTickTimer = s.airTickTimer;
    grounded = s.grounded;
    crouching = s.crouching;
    leanAmount = s.leanAmount;
    currentEyeHeight = s.eyeHeight;
    health = s.health;
    stance = s.stance;
    UpdateCamera();
}

void Player::Update(float dt, const World& world) {
    Simulate(dt, ReadInput(), world);
}

// Treat the player as a circle on the XZ plane against the box footprint
bool Player::OverlapsXZ(Vector3 pos, const BoundingBox& b) const {
    return pos.x > b.min.x - radius && pos.x < b.max.x + radius &&
           pos.z > b.min.z - radius && pos.z < b.max.z + radius;
}

bool Player::CollidesAt(Vector3 pos, const World& world) const {
    return CylinderCollides(pos, radius, GetHeight(), world);
}

// Highest surface under the player that is at or below their feet
float Player::FloorBelow(Vector3 pos, const World& world) const {
    float floor = 0.0f; // the ground plane
    for (const WorldBox& box : world) {
        BoundingBox b = box.Bounds();
        if (b.max.y <= pos.y + 0.05f && b.max.y > floor && OverlapsXZ(pos, b)) floor = b.max.y;
    }
    return floor;
}

// Lowest surface above the player's head
float Player::CeilingAbove(Vector3 pos, const World& world) const {
    float ceiling = 1e9f;
    for (const WorldBox& box : world) {
        BoundingBox b = box.Bounds();
        if (b.min.y >= pos.y + GetHeight() - 0.05f && b.min.y < ceiling && OverlapsXZ(pos, b)) ceiling = b.min.y;
    }
    return ceiling;
}

// Move v toward target by at most maxDelta (straight-line, no overshoot)
static Vector3 MoveTowards(Vector3 v, Vector3 target, float maxDelta) {
    Vector3 d = Vector3Subtract(target, v);
    float len = Vector3Length(d);
    if (len <= maxDelta || len < 1e-6f) return target;
    return Vector3Add(v, Vector3Scale(d, maxDelta / len));
}

// Source/Quake-style air acceleration. Pushes velocity along wishDir, but only
// until the velocity's component *along wishDir* reaches airWishSpeedCap.
//  - Holding W while already moving forward adds nothing (that component is
//    already above the cap), so you can't simply accelerate in the air.
//  - Holding A/D while moving forward pushes sideways, where that component is
//    ~0, so it always adds a little: the path bends. Turning the mouse with the
//    key keeps the push roughly perpendicular to your motion, which curves you
//    around and slowly builds speed - that's air strafing.
//  - Each tick adds at most airAccelerate * wishSpeed * tick, and never more
//    than airWishSpeedCap along wishDir, so there are no instant direction
//    changes; maxAirSpeed caps how fast strafing can get you.
void Player::AirAccelerate(Vector3 wishDir, float wishSpeed, float tick) {
    float speedBefore = GetSpeed();
    float capped = fminf(wishSpeed, airWishSpeedCap);
    float current = Vector3DotProduct(velocity, wishDir);
    float add = capped - current;
    if (add <= 0.0f) return;
    float accel = fminf(airAccelerate * wishSpeed * tick, add);
    velocity = Vector3Add(velocity, Vector3Scale(wishDir, accel));

    // Strafing can't take you past maxAirSpeed (but won't slow you down if
    // you were already going faster, e.g. off a sprint)
    float limit = fmaxf(maxAirSpeed, speedBefore);
    float speed = GetSpeed();
    if (speed > limit) velocity = Vector3Scale(velocity, limit / speed);
}

bool Player::TakeDamage(int amount) {
    if (health <= 0) return false;
    health = health > amount ? health - amount : 0;
    return health == 0;
}

Vector3 Player::GetRight() const {
    return { -cosf(yaw), 0.0f, sinf(yaw) };
}

// Where the eyes are for a given lean (-1..1)
Vector3 Player::HeadPosition(float lean) const {
    Vector3 head = { feet.x, feet.y + currentEyeHeight - leanDrop * fabsf(lean), feet.z };
    return Vector3Add(head, Vector3Scale(GetRight(), leanDistance * lean));
}

// Would the head (a small cylinder around the eyes) be inside something?
bool Player::HeadBlocked(float lean, const World& world) const {
    Vector3 head = HeadPosition(lean);
    return CylinderCollides({ head.x, head.y - 0.15f, head.z }, 0.15f, 0.2f, world);
}

void Player::Simulate(float dt, const PlayerInput& in, const World& world) {
    dt = fminf(dt, 0.05f); // avoid tunnelling through things on a long frame

    // --- Mouse look ---
    float sensitivity = mouseSensitivity * camera.fovy / baseFovy; // slower when zoomed
    float yawBefore = yaw; // air ticks inside this frame see the turn spread over the frame
    if (in.hasAim) {
        yaw = in.aimYaw;
        pitch = in.aimPitch;
    } else {
        yaw -= in.look.x * sensitivity;
        pitch -= in.look.y * sensitivity;
    }
    pitch = Clamp(pitch, -1.5f, 1.5f); // just under +/- 90 degrees

    // --- Crouching: instant hitbox change, smooth camera ---
    if (in.crouch) {
        crouching = true;
    } else if (crouching && !CylinderCollides(feet, radius, height, world)) {
        crouching = false; // only stand up if there's headroom
    }
    float targetEye = crouching ? crouchEyeHeight : eyeHeight;
    currentEyeHeight += (targetEye - currentEyeHeight) * (1.0f - expf(-crouchEaseRate * dt));

    // --- WASD movement on the XZ plane, with acceleration ---
    Vector3 forward = { sinf(yaw), 0.0f, cosf(yaw) };
    Vector3 right = GetRight();

    // Which way the keys point, relative to where you're facing
    float f = (in.forward ? 1.0f : 0.0f) - (in.back ? 1.0f : 0.0f);
    float s = (in.right ? 1.0f : 0.0f) - (in.left ? 1.0f : 0.0f);
    Vector3 target = { 0 }; // the velocity the keys are asking for
    float wishSpeed = 0.0f;
    if (f != 0.0f || s != 0.0f) {
        float len = sqrtf(f * f + s * s);
        f /= len;
        s /= len;
        // Strafing and backpedaling top out slower than running forward;
        // diagonals blend (f^2 + s^2 = 1). Sprint only counts moving forward.
        float dirScale = (f >= 0.0f ? f * f : f * f * backpedalScale) + s * s * strafeScale;
        float base = crouching ? crouchSpeed : (in.sprint && f > 0.0f ? sprintSpeed : walkSpeed);
        wishSpeed = base * dirScale;
        Vector3 dir = Vector3Add(Vector3Scale(forward, f), Vector3Scale(right, s));
        target = Vector3Scale(dir, wishSpeed);
    }
    bool wantsToMove = Vector3LengthSqr(target) > 0.0f;

    Vector3 oldVel = velocity;
    if (grounded) {
        airTickTimer = 0.0f;
        float rate = groundAccel;
        if (!wantsToMove) rate = groundDecel;                                   // let go: slide to a stop
        else if (Vector3DotProduct(velocity, target) < 0.0f) rate = groundAccel + groundDecel; // counter-strafe: brake hard
        velocity = MoveTowards(velocity, target, rate * dt);
    } else {
        // In the air: momentum carries you; the keys only add to it (see
        // AirAccelerate). Runs on a fixed tick so strafing gains the same
        // speed at any frame rate.
        const float tick = 1.0f / airTickRate;
        float tickTime = tick - airTickTimer; // when, within this frame, the next tick lands
        airTickTimer += dt;
        while (airTickTimer >= tick) {
            airTickTimer -= tick;
            if (wantsToMove) {
                // Where you were facing at that moment: this frame's mouse turn
                // is spread evenly over the frame
                float tickYaw = Lerp(yawBefore, yaw, Clamp(tickTime / dt, 0.0f, 1.0f));
                Vector3 tickForward = { sinf(tickYaw), 0.0f, cosf(tickYaw) };
                Vector3 tickRight = { -cosf(tickYaw), 0.0f, sinf(tickYaw) };
                Vector3 wishDir = Vector3Add(Vector3Scale(tickForward, f), Vector3Scale(tickRight, s));
                AirAccelerate(wishDir, wishSpeed, tick);
            }
            tickTime += tick;
        }
    }

    // Average of old and new velocity is exact for steady acceleration, so
    // distances don't depend on frame rate
    Vector3 move = Vector3Scale(Vector3Add(oldVel, velocity), 0.5f * dt);

    // Resolve each axis separately so you slide along walls; hitting one
    // kills your speed in that direction. On the ground, something low enough
    // (a stair) is stepped up onto instead.
    auto moveTo = [&](Vector3 to) {
        if (!CollidesAt(to, world)) { feet = to; return true; }
        if (!grounded) return false;
        Vector3 raised = { to.x, feet.y + stepHeight, to.z };
        if (CollidesAt(raised, world)) return false;
        float floor = FloorBelow(raised, world);
        if (floor <= feet.y + 0.01f) return false;
        stepSmooth -= floor - feet.y;   // the view eases up the step instead of jumping
        feet = { to.x, floor, to.z };
        return true;
    };
    if (!moveTo({ feet.x + move.x, feet.y, feet.z })) velocity.x = 0.0f;
    if (!moveTo({ feet.x, feet.y, feet.z + move.z })) velocity.z = 0.0f;

    // Walking down stairs: stay on them rather than falling off each step
    if (grounded && !in.jump && velocityY <= 0.0f) {
        float floor = FloorBelow(feet, world);
        float drop = feet.y - floor;
        if (drop > 0.001f && drop <= stepHeight) {
            stepSmooth += drop;
            feet.y = floor;
        }
    }
    stepSmooth *= expf(-16.0f * dt);

    // --- Jumping and gravity ---
    if (grounded && in.jump) {
        velocityY = jumpSpeed;
        grounded = false;
    }
    // Exact constant-acceleration step (y += v*dt - g*dt^2/2), so the jump arc
    // is the same at any frame rate. Plain "v -= g*dt; y += v*dt" jumps lower
    // the lower the frame rate.
    float newY = feet.y + velocityY * dt - 0.5f * gravity * dt * dt;
    velocityY -= gravity * dt;

    if (velocityY <= 0.0f) {
        float floor = FloorBelow(feet, world);
        grounded = newY <= floor;
        if (grounded) {
            newY = floor;
            velocityY = 0.0f;
        }
    } else {
        float ceiling = CeilingAbove(feet, world);
        if (newY + GetHeight() >= ceiling) { // bonk
            newY = ceiling - GetHeight();
            velocityY = 0.0f;
        }
        grounded = false;
    }
    feet.y = newY;

    // Stance follows how fast you're actually going, so stopping (or
    // counter-strafing) makes you accurate again once you've slowed down
    float speed = GetSpeed();
    bool moving = speed > 0.5f;
    if (!grounded) stance = Stance::Airborne;
    else if (crouching) stance = moving ? Stance::CrouchMoving : Stance::CrouchStill;
    else if (moving) stance = speed > walkSpeed + 0.25f ? Stance::Sprinting : Stance::Walking;
    else stance = Stance::Still;

    // --- Leaning: head slides sideways and the view tilts; feet stay put ---
    float leanTarget = (in.leanRight ? 1.0f : 0.0f) - (in.leanLeft ? 1.0f : 0.0f);
    float lean = leanAmount + (leanTarget - leanAmount) * (1.0f - expf(-leanEaseRate * dt));
    if (HeadBlocked(lean, world)) {
        // Lean only as far as there's room for the head
        float ok = 0.0f, bad = lean;
        for (int i = 0; i < 10; i++) {
            float mid = (ok + bad) * 0.5f;
            if (HeadBlocked(mid, world)) bad = mid; else ok = mid;
        }
        lean = ok;
    }
    leanAmount = lean;

    UpdateCamera();
}

void Player::UpdateCamera() {
    camera.position = Vector3Add(HeadPosition(leanAmount), { 0.0f, stepSmooth, 0.0f });
    camera.target = Vector3Add(camera.position, GetLookDirection());
    float roll = leanRollDegrees * leanAmount * DEG2RAD; // tilt toward the lean
    camera.up = Vector3Add(Vector3Scale({ 0.0f, 1.0f, 0.0f }, cosf(roll)), Vector3Scale(GetRight(), sinf(roll)));
}
