// Route wiring (frozen after Wave 0). Each page lives in src/pages/<name>/ and is owned by its
// workstream; this file only maps URLs to pages.
import type { RouteObject } from 'react-router';
import { AppShell } from './shell/AppShell';
import LoginPage from './pages/login';
import SignupPage from './pages/signup';
import OnboardingPage from './pages/onboarding';
import ForYouPage from './pages/for-you';
import ComparePage from './pages/compare';
import MoviePage from './pages/movie';
import RatingsPage from './pages/ratings';
import EvaluationPage from './pages/evaluation';
import HowItWorksPage from './pages/how-it-works';

export const routes: RouteObject[] = [
  {
    element: <AppShell />,
    children: [
      { path: '/login', element: <LoginPage /> },
      { path: '/signup', element: <SignupPage /> },
      { path: '/onboarding', element: <OnboardingPage /> },
      { path: '/', element: <ForYouPage /> },
      { path: '/compare', element: <ComparePage /> },
      { path: '/movies/:movieId', element: <MoviePage /> },
      { path: '/ratings', element: <RatingsPage /> },
      { path: '/evaluation', element: <EvaluationPage /> },
      { path: '/how-it-works', element: <HowItWorksPage /> },
    ],
  },
];
