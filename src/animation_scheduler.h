#ifndef FALLOUT_ANIMATION_SCHEDULER_H_
#define FALLOUT_ANIMATION_SCHEDULER_H_

namespace fallout {

// Phase 2.3 animation timing seam (REWRITE_PLAN 2.3).
//
// The animation engine (animation.cc) advances frames through this scheduler
// instead of gating directly on the wall clock. The BASE class is the real-time
// client behavior: a frame advances once its per-frame interval has elapsed, one
// frame per engine pump. That makes the seam a no-op for the normal client, and
// a no-op to introduce (nothing installs a scheduler -> the base runs).
//
// InstantAnimationScheduler (headless probe / future f2_server) completes
// sequences instantly: every terminating animation advances on every pass and
// the engine drains all sads within a single pump. Completion callbacks still
// fire through the unchanged animationRunSequence/_anim_set_end path in the same
// registered order -- ordering, not timing, is the preserved contract.
//
// The scheduler owns ONLY the frame-advance cadence (the two policies below); it
// never touches engine state. All timing values are passed in by value, so this
// header has no SDL / getTicks dependency and is safe in f2_core.
class AnimationScheduler {
public:
    virtual ~AnimationScheduler() = default;

    // Should the sad whose last frame advanced at [timestamp], with per-frame
    // interval [ticksPerFrame], advance now (current tick [now])? [looping] is
    // true for ANIM_SAD_FOREVER/idle animations. Base = real-time: advance once
    // the interval has elapsed. This is the original animation.cc gate
    // `!(getTicksBetween(now, timestamp) < ticksPerFrame)` inlined; the unsigned
    // subtraction matches getTicksBetween's start>end (wrap) case, which also
    // advances.
    virtual bool frameReady(unsigned int timestamp, unsigned int ticksPerFrame, unsigned int now, bool looping)
    {
        return now - timestamp >= ticksPerFrame;
    }

    // After a full pass over all sads, repeat the pass now (drain to completion)
    // or yield to the next engine pump? Base = real-time: yield (one frame/pump).
    virtual bool drainWithinPump()
    {
        return false;
    }
};

// Completes animations instantly while preserving completion-callback order.
// Never advances looping (FOREVER/idle) animations -- they fire no sim callback
// and would keep a drain from terminating.
class InstantAnimationScheduler : public AnimationScheduler {
public:
    bool frameReady(unsigned int timestamp, unsigned int ticksPerFrame, unsigned int now, bool looping) override
    {
        return !looping;
    }

    bool drainWithinPump() override
    {
        return true;
    }
};

// Current scheduler; never null (defaults to the real-time base).
AnimationScheduler* animationScheduler();

// Install a scheduler (nullptr restores the real-time base).
void animationSchedulerSet(AnimationScheduler* newScheduler);

} // namespace fallout

#endif /* FALLOUT_ANIMATION_SCHEDULER_H_ */
