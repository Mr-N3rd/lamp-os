import 'package:go_router/go_router.dart';
import 'package:riverpod_annotation/riverpod_annotation.dart';

import '../../features/control/presentation/knockout_screen.dart';
import '../../features/lamp_shell/presentation/expression_editor_screen.dart';
import '../../features/inventory/application/active_lamp_notifier.dart';
import '../../features/inventory/application/inventory_notifier.dart';
import '../../features/lamp_shell/presentation/lamp_shell.dart';
import '../../features/nearby/presentation/nearby_lamps_screen.dart';
import '../../features/onboarding/presentation/add_lamp_shell.dart';
import '../../features/onboarding/presentation/onboarding_placeholder.dart';
import 'routes.dart';

part 'router.g.dart';

@Riverpod(keepAlive: true, name: 'appRouterProvider')
GoRouter appRouter(Ref ref) {
  return GoRouter(
    initialLocation: AppRoutes.onboarding,
    redirect: (context, state) {
      final inv = ref.read(inventoryNotifierProvider).value;
      final active = ref.read(activeLampNotifierProvider).value;
      if (inv == null) return null; // still loading
      final loc = state.uri.toString();
      // Empty inventory → land on onboarding, but allow the user to
      // navigate forward into the AddLamp flow and the debug devices view.
      if (inv.isEmpty) {
        if (loc.startsWith('/onboarding') || loc.startsWith('/devices')) {
          return null;
        }
        return AppRoutes.onboarding;
      }
      final target = active ?? inv.first.id;
      if (loc == AppRoutes.onboarding) {
        return AppRoutes.control(target);
      }
      return null;
    },
    routes: [
      GoRoute(
        path: AppRoutes.onboarding,
        builder: (_, _) => const OnboardingPlaceholder(),
      ),
      GoRoute(
        path: '/lamp/:id/control',
        builder: (_, state) =>
            LampShell(lampId: state.pathParameters['id']!),
      ),
      GoRoute(
        path: '/lamp/:id/expressions',
        builder: (_, state) => LampShell(
          lampId: state.pathParameters['id']!,
          initialTab: LampTab.expressions,
        ),
      ),
      GoRoute(
        path: '/lamp/:id/setup',
        builder: (_, state) => LampShell(
          lampId: state.pathParameters['id']!,
          initialTab: LampTab.setup,
        ),
      ),
      GoRoute(
        path: '/onboarding/add',
        builder: (_, _) => const AddLampShell(),
      ),
      GoRoute(
        path: '/devices',
        builder: (_, state) => const NearbyLampsScreen(),
      ),
      GoRoute(
        path: '/lamp/:id/control/knockout',
        builder: (_, state) =>
            KnockoutScreen(lampId: state.pathParameters['id']!),
      ),
      GoRoute(
        path: '/lamp/:id/expressions/:type/:target',
        builder: (_, state) => ExpressionEditorScreen(
          lampId: state.pathParameters['id']!,
          typeKey: state.pathParameters['type']!,
          targetKey: int.parse(state.pathParameters['target']!),
        ),
      ),
    ],
  );
}
