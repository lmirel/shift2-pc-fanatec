#include "../src/bridge.hpp"

static void check(bool condition, const char* name)
{
    if (!condition) {
        fprintf(stderr, "FAIL %s\n", name);
        exit(1);
    }
}
template<class T>
static void parameters(EffectState& effect, const T& input)
{
    const auto first = reinterpret_cast<const BYTE*>(&input);
    effect.parameters.assign(first, first + sizeof(input));
}
int main()
{
    EffectState effect;
    effect.playing = true;
    effect.started = 1000;
    effect.type = GUID_ConstantForce;
    parameters(effect, DICONSTANTFORCE{5000});
    WheelSample sample;
    check(std::abs(evaluate_effect(effect, sample, 1001) - 0.5) < 1e-6, "constant magnitude");
    effect.gain = 5000;
    check(std::abs(evaluate_effect(effect, sample, 1001) - 0.25) < 1e-6, "effect gain");
    effect.gain = 10000;
    effect.direction = -10000;
    check(evaluate_effect(effect, sample, 1001) < 0, "direction reversal");
    effect.direction = 10000;
    effect.duration = 100000;
    check(evaluate_effect(effect, sample, 1100) == 0, "finite duration");
    effect.iterations = 2;
    check(evaluate_effect(effect, sample, 1150) > 0, "effect repeats");
    check(evaluate_effect(effect, sample, 1200) == 0, "repeat end");
    effect.iterations = 1;
    effect.type = GUID_RampForce;
    parameters(effect, DIRAMPFORCE{0,10000});
    check(std::abs(evaluate_effect(effect, sample, 1050) - 0.5) < 1e-6, "ramp midpoint");
    effect.type = GUID_Sine;
    effect.duration = INFINITE;
    parameters(effect, DIPERIODIC{10000,0,0,100000});
    check(std::abs(evaluate_effect(effect, sample, 1025) - 1) < 1e-6, "sine quarter period");
    effect.type = GUID_Spring;
    DICONDITION spring{0,5000,5000,10000,10000,0};
    parameters(effect, spring);
    sample.axes[0] = 65535;
    check(evaluate_effect(effect, sample, 1001) < 0, "spring opposes positive displacement");
    sample.axes[0] = 0;
    check(evaluate_effect(effect, sample, 1001) > 0, "spring opposes negative displacement");
    effect.downloaded = false;
    check(evaluate_effect(effect, sample, 1001) == 0, "unloaded effect silent");
    puts("PASS: force magnitude, gain, direction, lifetime, repeats, ramp, periodic, spring and unload.");
}
