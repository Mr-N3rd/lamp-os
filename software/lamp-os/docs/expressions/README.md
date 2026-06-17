# Expressions System

Expressions are animated behaviors that give the lamp personality through time-triggered visual effects.

## Architecture

### Core Concepts

- **Expressions** - Inherit from `Expression` base class which provides timing, triggering, and lifecycle management
- **Frame Buffers** - Each expression operates on a specific buffer (shade, base, or both)
- **Composability** - Multiple expressions can blend together for layered effects
- **Exclusivity** - Some expressions can take exclusive control when active

### Expression Lifecycle

Each expression follows a simple lifecycle:

1. **Trigger** - Either automatic (based on interval) or manual (from UI/other expressions)
2. **Animation** - Plays for a configured duration
3. **Completion** - Cleanup and scheduling of next trigger

### Lifecycle Hooks

Expressions implement three key methods:

```cpp
class MyExpression : public Expression {
protected:
  void onTrigger() override {
    // Setup when triggered (REQUIRED)
    // Initialize colors, state, etc.
  }

  void onUpdate() override {
    // Per-frame updates (OPTIONAL)
    // Called while animating
  }

  void onComplete() override {
    // Cleanup when done (OPTIONAL)
    // Trigger other expressions, etc.
  }

  void draw() override {
    // Render to buffer (REQUIRED)
    // Apply visual effect
  }
};
```

### Configuration

Expressions are configured with:
- **Colors** - Palette of colors to use
- **Interval** - Min/max seconds between automatic triggers
- **Target** - Which buffer to affect (shade/base/both)
- **Custom Parameters** - Expression-specific settings

## Included Expressions

LampOS keeps the catalog intentionally small and calm. Current built-in expressions:

| Expression | Style | Home mode | Key parameters |
| --- | --- | --- | --- |
| Glitchy | brief disruptive tint glitch | no | duration range |
| Shifty | slow ambient palette shift | yes | fade duration, shifted hold range |
| Pulse | one soft wave pass | yes | pulse speed |
| Palette Cycle | slow rotation through a configured palette | yes | step duration |
| Twinkle | a few soft sparkles at a time | yes | active twinkles, twinkle fade, run time |
| Comet | gentle chase with a trailing tail | yes | travel time, tail length |
| Candle | warm flicker with restrained brightness movement | yes | flicker amount, run time |

### Notes on the lighter effects

- **Palette Cycle** works best with 2-5 colors and long intervals for a slow living-room feel.
- **Twinkle** is tuned for small LED counts, so only a few pixels light up at once.
- **Comet** is intentionally softer than a WLED-style chase and defaults to a single relaxed pass.
- **Candle** is best paired with amber or warm-white palettes, but still respects any chosen expression colors.

### Triggering

Expressions can be triggered:
- **Automatically** - Based on configured interval
- **Manually** - Via UI or WebSocket
- **Programmatically** - By other expressions

Expressions handle re-triggers gracefully, canceling current animations and starting fresh.

## Creating New Expressions

See [skeleton_expression.md](skeleton_expression.md) for a minimal example to get started.