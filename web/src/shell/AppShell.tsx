// Page frame shared by every route: header navigation, account menu, the model-unavailable
// banner, and the attribution footer. It is the root route element, so it also provides the
// session to every page.
import { Link, NavLink, Outlet } from 'react-router';
import { SessionProvider } from '../auth/session';
import { AccountMenu } from './AccountMenu';
import { ModelUnavailableBanner } from './ModelUnavailableBanner';

const NAV = [
  { to: '/', label: 'For you' },
  { to: '/compare', label: 'Compare' },
  { to: '/ratings', label: 'My ratings' },
  { to: '/evaluation', label: 'Evaluation' },
  { to: '/how-it-works', label: 'How it works' },
];

export function AppShell() {
  return (
    <SessionProvider>
      <div className="shell">
        <a className="skip-link" href="#main">
          Skip to content
        </a>
        <header className="shell-header">
          <div className="shell-header__inner">
            <Link to="/" className="brand">
              Intermission
            </Link>
            <nav aria-label="Main">
              {NAV.map((item) => (
                <NavLink key={item.to} to={item.to} end={item.to === '/'}>
                  {item.label}
                </NavLink>
              ))}
            </nav>
            <AccountMenu />
          </div>
        </header>
        <ModelUnavailableBanner />
        <main id="main" tabIndex={-1}>
          <Outlet />
        </main>
        <footer className="shell-footer">
          <p>Data from MovieLens by GroupLens Research; not endorsed by the University of Minnesota or GroupLens.</p>
          <p>This product uses the TMDB API but is not endorsed or certified by TMDB.</p>
        </footer>
      </div>
    </SessionProvider>
  );
}
