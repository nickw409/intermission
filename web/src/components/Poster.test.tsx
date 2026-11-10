import { render, screen } from '@testing-library/react';
import { describe, expect, it } from 'vitest';
import { axeViolations } from '../test/axe';
import { Poster } from './Poster';

describe('Poster', () => {
  it('is a same-origin image with the title and year as alt text', () => {
    render(<Poster movieId={2571} title="Matrix, The" year={1999} />);
    const img = screen.getByRole('img', { name: 'The Matrix (1999)' });
    expect(img.tagName).toBe('IMG');
    expect(img).toHaveAttribute('src', '/api/posters/2571');
  });

  it.each([0, -3, 1.5, 2147483648])('renders a labelled placeholder, not a crash, for movie id %s', async (id) => {
    const { container } = render(<Poster movieId={id} title="Matrix, The" year={1999} />);
    const placeholder = screen.getByRole('img', { name: 'The Matrix (1999)' });
    expect(placeholder.tagName).not.toBe('IMG');
    expect(container.querySelector('img')).toBeNull();
    expect(await axeViolations(container)).toEqual([]);
  });
});
