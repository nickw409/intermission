// Labelled text input with an optional hint. When there is an error it replaces the hint, is
// linked through aria-describedby, and the input is marked aria-invalid.
import { useId, type InputHTMLAttributes, type Ref } from 'react';

interface TextFieldProps extends Omit<InputHTMLAttributes<HTMLInputElement>, 'onChange' | 'value'> {
  label: string;
  value: string;
  onChange: (value: string) => void;
  hint?: string | undefined;
  error?: string | undefined;
  ref?: Ref<HTMLInputElement>;
}

export function TextField({ label, value, onChange, hint, error, ref, ...input }: TextFieldProps) {
  const id = useId();
  const describedById = `${id}-description`;
  const description = error ?? hint;
  return (
    <div className={error ? 'field field--invalid' : 'field'}>
      <label htmlFor={id}>{label}</label>
      <input
        {...input}
        id={id}
        ref={ref}
        value={value}
        onChange={(event) => onChange(event.target.value)}
        aria-invalid={error ? true : undefined}
        aria-describedby={description ? describedById : undefined}
      />
      {description && (
        <p id={describedById} className={error ? 'field__error' : 'field__hint'}>
          {description}
        </p>
      )}
    </div>
  );
}
