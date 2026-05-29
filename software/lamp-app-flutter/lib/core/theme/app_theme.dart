import 'package:flutter/material.dart';

import 'brand_colors.dart';

abstract class AppTheme {
  static ThemeData dark() {
    const colorScheme = ColorScheme.dark(
      primary: BrandColors.auroraBlue,
      onPrimary: BrandColors.lampWhite,
      secondary: BrandColors.glowPink,
      onSecondary: BrandColors.midnightBlack,
      tertiary: BrandColors.lumenGreen,
      onTertiary: BrandColors.midnightBlack,
      surface: BrandColors.midnightBlack,
      onSurface: BrandColors.lampWhite,
      error: Color(0xFFE57373),
      onError: BrandColors.lampWhite,
    );

    return ThemeData(
      useMaterial3: true,
      brightness: Brightness.dark,
      colorScheme: colorScheme,
      scaffoldBackgroundColor: BrandColors.midnightBlack,
      canvasColor: BrandColors.midnightBlack,
      cardTheme: const CardThemeData(
        color: Color(0x0AFDFDFD),
        elevation: 0,
        shape: RoundedRectangleBorder(
          borderRadius: BorderRadius.all(Radius.circular(12)),
          side: BorderSide(color: Color(0x0FFDFDFD)),
        ),
      ),
      dividerTheme: const DividerThemeData(
        color: Color(0x14FDFDFD),
        thickness: 1,
      ),
      sliderTheme: const SliderThemeData(
        thumbColor: BrandColors.lampWhite,
        activeTrackColor: BrandColors.glowPink,
        inactiveTrackColor: BrandColors.ashGrey,
      ),
      textTheme: const TextTheme(
        bodyLarge: TextStyle(color: BrandColors.lampWhite),
        bodyMedium: TextStyle(color: BrandColors.fogGrey),
        labelSmall: TextStyle(
          color: BrandColors.slateGrey,
          letterSpacing: 0.05,
        ),
      ),
    );
  }
}
