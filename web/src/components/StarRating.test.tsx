import { render, screen } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { useState } from 'react';
import { describe, expect, it, vi } from 'vitest';
import { axeViolations } from '../test/axe';
import { StarRating } from './StarRating';

function Controlled({ initial, onCommit }: { initial: number | null; onCommit?: (v: number | null) => void }) {
  const [value, setValue] = useState(initial);
  return (
    <StarRating
      label="Your rating"
      value={value}
      onChange={(v) => {
        setValue(v);
        onCommit?.(v);
      }}
    />
  );
}

describe('StarRating', () => {
  it('is a single focusable slider with 0.5–5 range and "N stars" value text', () => {
    render(<StarRating label="Your rating" value={3.5} onChange={() => {}} />);
    const slider = screen.getByRole('slider', { name: 'Your rating' });
    expect(slider).toHaveAttribute('tabindex', '0');
    expect(slider).toHaveAttribute('aria-valuemin', '0.5');
    expect(slider).toHaveAttribute('aria-valuemax', '5');
    expect(slider).toHaveAttribute('aria-valuenow', '3.5');
    expect(slider).toHaveAttribute('aria-valuetext', '3.5 stars');
    expect(screen.getAllByRole('slider')).toHaveLength(1);
  });

  it('describes an unrated value as "Not rated" without aria-valuenow', () => {
    render(<StarRating label="Your rating" value={null} onChange={() => {}} />);
    const slider = screen.getByRole('slider');
    expect(slider).not.toHaveAttribute('aria-valuenow');
    expect(slider).toHaveAttribute('aria-valuetext', 'Not rated');
  });

  it('uses the singular for one star', () => {
    render(<StarRating label="Your rating" value={1} onChange={() => {}} />);
    expect(screen.getByRole('slider')).toHaveAttribute('aria-valuetext', '1 star');
  });

  it('arrow keys move by 0.5 and Enter commits', async () => {
    const user = userEvent.setup();
    const onCommit = vi.fn();
    render(<Controlled initial={3} onCommit={onCommit} />);
    const slider = screen.getByRole('slider');
    await user.click(slider);
    onCommit.mockClear(); // a click may commit; this test is about the keyboard

    slider.focus();
    await user.keyboard('{ArrowRight}');
    expect(slider).toHaveAttribute('aria-valuenow', '3.5');
    await user.keyboard('{ArrowUp}');
    expect(slider).toHaveAttribute('aria-valuenow', '4');
    await user.keyboard('{ArrowLeft}{ArrowLeft}{ArrowDown}');
    expect(slider).toHaveAttribute('aria-valuenow', '2.5');
    expect(slider).toHaveAttribute('aria-valuetext', '2.5 stars');
    expect(onCommit).not.toHaveBeenCalled();

    await user.keyboard('{Enter}');
    expect(onCommit).toHaveBeenCalledExactlyOnceWith(2.5);
  });

  it('Space commits, and keyboard changes stay within 0.5–5', async () => {
    const user = userEvent.setup();
    const onCommit = vi.fn();
    render(<Controlled initial={4.5} onCommit={onCommit} />);
    const slider = screen.getByRole('slider');
    slider.focus();
    await user.keyboard('{ArrowRight}{ArrowRight}{ArrowRight}');
    expect(slider).toHaveAttribute('aria-valuenow', '5');
    await user.keyboard(' ');
    expect(onCommit).toHaveBeenLastCalledWith(5);

    await user.keyboard('{Home}');
    expect(slider).toHaveAttribute('aria-valuenow', '0.5');
    await user.keyboard('{ArrowLeft}');
    expect(slider).toHaveAttribute('aria-valuenow', '0.5');
    await user.keyboard('{Enter}');
    expect(onCommit).toHaveBeenLastCalledWith(0.5);

    await user.keyboard('{End}{Enter}');
    expect(onCommit).toHaveBeenLastCalledWith(5);
  });

  it('starts keyboard selection at 0.5 when unrated', async () => {
    const user = userEvent.setup();
    const onCommit = vi.fn();
    render(<Controlled initial={null} onCommit={onCommit} />);
    const slider = screen.getByRole('slider');
    slider.focus();
    await user.keyboard('{ArrowRight}');
    expect(slider).toHaveAttribute('aria-valuenow', '0.5');
    await user.keyboard('{ArrowRight}{Enter}');
    expect(onCommit).toHaveBeenCalledExactlyOnceWith(1);
  });

  it('discards an uncommitted keyboard change on blur', async () => {
    const user = userEvent.setup();
    const onCommit = vi.fn();
    render(
      <>
        <Controlled initial={2} onCommit={onCommit} />
        <button type="button">elsewhere</button>
      </>,
    );
    const slider = screen.getByRole('slider');
    slider.focus();
    await user.keyboard('{ArrowRight}{ArrowRight}');
    expect(slider).toHaveAttribute('aria-valuenow', '3');
    await user.tab();
    expect(slider).toHaveAttribute('aria-valuenow', '2');
    expect(onCommit).not.toHaveBeenCalled();
  });

  it('Delete or Backspace clears the rating', async () => {
    const user = userEvent.setup();
    const onCommit = vi.fn();
    render(<Controlled initial={4} onCommit={onCommit} />);
    const slider = screen.getByRole('slider');
    slider.focus();
    await user.keyboard('{Delete}');
    expect(onCommit).toHaveBeenLastCalledWith(null);
    expect(slider).toHaveAttribute('aria-valuetext', 'Not rated');

    await user.keyboard('{End}{Enter}{Backspace}');
    expect(onCommit).toHaveBeenLastCalledWith(null);
  });

  it('has a clear button when rated', async () => {
    const user = userEvent.setup();
    const onCommit = vi.fn();
    render(<Controlled initial={4} onCommit={onCommit} />);
    await user.click(screen.getByRole('button', { name: 'Clear rating' }));
    expect(onCommit).toHaveBeenCalledExactlyOnceWith(null);
    expect(screen.queryByRole('button', { name: 'Clear rating' })).not.toBeInTheDocument();
  });

  it('commits the half star that was clicked', async () => {
    const user = userEvent.setup();
    const onCommit = vi.fn();
    const { container } = render(<Controlled initial={null} onCommit={onCommit} />);
    const half = container.querySelector('[data-star-value="3.5"]');
    expect(half).not.toBeNull();
    await user.click(half as Element);
    expect(onCommit).toHaveBeenCalledExactlyOnceWith(3.5);
    expect(screen.getByRole('slider')).toHaveAttribute('aria-valuenow', '3.5');
  });

  it('readonly mode shows the value but cannot be focused or changed', async () => {
    const user = userEvent.setup();
    const onChange = vi.fn();
    const { container } = render(<StarRating label="Average rating" value={4.5} readOnly onChange={onChange} />);
    expect(screen.queryByRole('slider')).not.toBeInTheDocument();
    const img = screen.getByRole('img', { name: 'Average rating: 4.5 stars' });
    expect(img).not.toHaveAttribute('tabindex');
    expect(screen.queryByRole('button')).not.toBeInTheDocument();
    const half = container.querySelector('[data-star-value="1"]');
    if (half) await user.click(half);
    expect(onChange).not.toHaveBeenCalled();
  });

  it('has no axe violations in interactive and readonly modes', async () => {
    // Inside a landmark, as on every real page (the shell's <main>).
    render(
      <main>
        <StarRating label="Your rating" value={2.5} onChange={() => {}} />
        <StarRating label="Average rating" value={4} readOnly />
      </main>,
    );
    expect(await axeViolations()).toEqual([]);
  });
});
