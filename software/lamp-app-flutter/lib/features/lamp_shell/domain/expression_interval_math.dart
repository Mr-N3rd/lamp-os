import 'dart:math' as math;

class ExpressionIntervalRange {
  const ExpressionIntervalRange(this.min, this.max);
  final int min;
  final int max;
}

class ExpressionIntervalNorms {
  const ExpressionIntervalNorms({required this.freq, required this.spread});
  final double freq;
  final double spread;
}

abstract class ExpressionIntervalMath {
  static const int minSec = 10;
  static const int maxSec = 3600;
  static const double maxMultiplier = 5.0;

  static ExpressionIntervalRange intervalFromNorms(double freq, double spread) {
    final f = freq.clamp(0.0, 1.0);
    final s = spread.clamp(0.0, 1.0);
    final logLo = math.log(minSec.toDouble());
    final logHi = math.log(maxSec.toDouble());
    final centre = math.exp(logHi + (logLo - logHi) * f);
    final mult = 1.0 + s * (maxMultiplier - 1.0);
    final rawLo = (centre / mult).clamp(minSec.toDouble(), maxSec.toDouble());
    final rawHi = (centre * mult).clamp(minSec.toDouble(), maxSec.toDouble());
    final lo = rawLo.round();
    final hi = rawHi.round();
    return ExpressionIntervalRange(math.min(lo, hi), math.max(lo, hi));
  }

  static ExpressionIntervalNorms normsFromInterval(int intervalMin, int intervalMax) {
    final loRaw = intervalMin.clamp(1, 1 << 30).toDouble();
    final hiRaw = intervalMax.clamp(1, 1 << 30).toDouble();
    final lo = math.min(loRaw, hiRaw);
    final hi = math.max(loRaw, hiRaw);
    final centre = math.sqrt(lo * hi);
    final mult = math.sqrt(hi / lo);
    final logLo = math.log(minSec.toDouble());
    final logHi = math.log(maxSec.toDouble());
    final f = ((math.log(centre) - logHi) / (logLo - logHi)).clamp(0.0, 1.0);
    final s = ((mult - 1.0) / (maxMultiplier - 1.0)).clamp(0.0, 1.0);
    return ExpressionIntervalNorms(freq: f, spread: s);
  }
}
