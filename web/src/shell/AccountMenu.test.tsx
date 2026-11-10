import { screen, within } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { describe, expect, it } from 'vitest';
import { recordRequests } from '../lib/test-helpers';
import { handlerFor } from '../mocks/handlers';
import { axeViolations } from '../test/axe';
import { renderApp } from '../test/render';
import { server } from '../test/setup';

const RETENTION =
  'Copies in nightly backups and training exports are removed on a rolling schedule, normally within 5 weeks.';

/** Signs in as alice (getMe fixture), opens the account menu and the delete-account dialog. */
async function openDeleteDialog(path = '/ratings') {
  const user = userEvent.setup();
  const app = renderApp(path);
  await user.click(await screen.findByRole('button', { name: /alice/ }));
  await user.click(screen.getByRole('button', { name: 'Delete account' }));
  const dialog = screen.getByRole('dialog', { name: 'Delete your account?' });
  return { ...app, user, dialog };
}

const confirmInput = (dialog: HTMLElement) => within(dialog).getByLabelText('Type your username to confirm');
const confirmButton = (dialog: HTMLElement) => within(dialog).getByRole('button', { name: 'Delete my account' });

describe('delete account', () => {
  it('is offered in the account menu', async () => {
    const user = userEvent.setup();
    renderApp('/');
    await user.click(await screen.findByRole('button', { name: /alice/ }));
    expect(screen.getByRole('button', { name: 'Delete account' })).toBeVisible();
  });

  it('opens a labelled modal dialog with the retention note, focused on the confirmation field', async () => {
    const { dialog } = await openDeleteDialog();
    expect(dialog).toHaveAttribute('aria-modal', 'true');
    expect(dialog).toHaveTextContent('Your account and all your ratings are deleted immediately.');
    expect(dialog).toHaveTextContent(RETENTION);
    expect(dialog).toHaveAccessibleDescription(expect.stringContaining(RETENTION) as string);
    expect(confirmInput(dialog)).toHaveFocus();
  });

  it('keeps the delete button disabled until the username is typed exactly', async () => {
    const seen = recordRequests('/api/me', 'DELETE');
    const { user, dialog } = await openDeleteDialog();
    const input = confirmInput(dialog);
    const button = confirmButton(dialog);
    expect(button).toBeDisabled();
    await user.type(input, 'alic');
    expect(button).toBeDisabled();
    await user.clear(input);
    await user.type(input, 'Alice');
    expect(button).toBeDisabled();
    await user.clear(input);
    await user.type(input, 'alice ');
    expect(button).toBeDisabled();
    await user.keyboard('{Enter}'); // submitting the form early does nothing either
    expect(seen).toHaveLength(0);
    await user.clear(input);
    await user.type(input, 'alice');
    expect(button).toBeEnabled();
    expect(seen).toHaveLength(0);
  });

  it('deletes through DELETE /api/me once, signs out and shows a confirmation on the login page', async () => {
    const seen = recordRequests('/api/me', 'DELETE');
    const { user, dialog, router } = await openDeleteDialog();
    await user.type(confirmInput(dialog), 'alice');
    await user.click(confirmButton(dialog));
    expect(await screen.findByText('Your account has been deleted.')).toBeInTheDocument();
    expect(router.state.location.pathname).toBe('/login');
    expect(screen.getByRole('heading', { level: 1, name: 'Log in' })).toBeInTheDocument();
    expect(screen.getByRole('link', { name: 'Log in' })).toBeInTheDocument();
    expect(screen.queryByRole('button', { name: /alice/ })).not.toBeInTheDocument();
    expect(screen.queryByRole('dialog')).not.toBeInTheDocument();
    expect(seen).toHaveLength(1);
    expect(seen[0]?.headers.get('X-Requested-With')).toBe('recs');
  });

  it('treats a 401 as already signed out', async () => {
    server.use(handlerFor('deleteMe', 'unauthorized'));
    const { user, dialog, router } = await openDeleteDialog();
    await user.type(confirmInput(dialog), 'alice');
    await user.click(confirmButton(dialog));
    expect(await screen.findByRole('link', { name: 'Log in' })).toBeInTheDocument();
    expect(screen.queryByRole('button', { name: /alice/ })).not.toBeInTheDocument();
    expect(router.state.location.pathname).toBe('/login');
    // Wait for the login page itself; checking earlier would pass before it could show the notice.
    expect(await screen.findByRole('heading', { level: 1, name: 'Log in' })).toBeInTheDocument();
    expect(screen.queryByText('Your account has been deleted.')).not.toBeInTheDocument();
  });

  it('shows the error and stays signed in when deletion fails', async () => {
    server.use(handlerFor('deleteMe', 'csrf_rejected'));
    const { user, dialog, router } = await openDeleteDialog();
    await user.type(confirmInput(dialog), 'alice');
    await user.click(confirmButton(dialog));
    expect(await within(dialog).findByRole('alert')).toHaveTextContent(
      'Your request was blocked. Reload the page and try again.',
    );
    expect(screen.getByRole('dialog', { name: 'Delete your account?' })).toBeInTheDocument();
    expect(screen.getByRole('button', { name: /alice/ })).toBeInTheDocument();
    expect(screen.queryByRole('link', { name: 'Log in' })).not.toBeInTheDocument();
    expect(router.state.location.pathname).toBe('/ratings');
    expect(confirmButton(dialog)).toBeEnabled(); // can try again
  });

  it('Escape cancels without a request and returns focus to the Delete account button', async () => {
    const seen = recordRequests('/api/me', 'DELETE');
    const { user, dialog } = await openDeleteDialog();
    await user.type(confirmInput(dialog), 'alice');
    await user.keyboard('{Escape}');
    expect(screen.queryByRole('dialog')).not.toBeInTheDocument();
    expect(screen.getByRole('button', { name: 'Delete account' })).toHaveFocus();
    expect(screen.getByRole('button', { name: /alice/ })).toBeInTheDocument();
    expect(seen).toHaveLength(0);
  });

  it('Cancel closes the dialog without a request, and reopening starts from an empty field', async () => {
    const seen = recordRequests('/api/me', 'DELETE');
    const { user, dialog } = await openDeleteDialog();
    await user.type(confirmInput(dialog), 'alice');
    await user.click(within(dialog).getByRole('button', { name: 'Cancel' }));
    expect(screen.queryByRole('dialog')).not.toBeInTheDocument();
    expect(screen.getByRole('button', { name: 'Delete account' })).toHaveFocus();
    await user.click(screen.getByRole('button', { name: 'Delete account' }));
    const again = screen.getByRole('dialog', { name: 'Delete your account?' });
    expect(confirmInput(again)).toHaveValue('');
    expect(confirmButton(again)).toBeDisabled();
    expect(seen).toHaveLength(0);
  });

  it('keeps keyboard focus inside the dialog', async () => {
    const { user, dialog } = await openDeleteDialog();
    await user.type(confirmInput(dialog), 'alice');
    // Field → Cancel → Delete my account → wraps back to the field, and Shift+Tab wraps the other way.
    await user.tab();
    expect(within(dialog).getByRole('button', { name: 'Cancel' })).toHaveFocus();
    await user.tab();
    expect(confirmButton(dialog)).toHaveFocus();
    await user.tab();
    expect(confirmInput(dialog)).toHaveFocus();
    await user.tab({ shift: true });
    expect(confirmButton(dialog)).toHaveFocus();
  });

  it('has no axe violations with the dialog open', async () => {
    await openDeleteDialog('/how-it-works');
    expect(await axeViolations()).toEqual([]);
  });
});
