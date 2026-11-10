import { render } from '@testing-library/react';
import { expect, it } from 'vitest';
import { axeViolations } from './axe';

it('reports a violation for an unlabelled input and a slider without a name', async () => {
  render(
    <main>
      <input />
      <span role="slider" tabIndex={0} aria-valuenow={1} />
    </main>,
  );
  const ids = (await axeViolations()).map((v) => v.id);
  expect(ids).toContain('label');
  expect(ids).toContain('aria-input-field-name');
});
