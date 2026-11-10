// Client-side checks that mirror contracts/public.yaml (SignupRequest, LoginRequest). They only
// save a round trip; the server enforces the same rules. Lengths count Unicode code points, as
// JSON Schema's minLength/maxLength do.

export const USERNAME_MIN = 3;
export const USERNAME_MAX = 32;
export const USERNAME_PATTERN = /^[A-Za-z0-9_.-]+$/;
export const PASSWORD_MIN = 10;
export const PASSWORD_MAX = 128;
export const LOGIN_USERNAME_MAX = 64;
export const INVITE_MAX = 64;

export const USERNAME_RULE = `Use ${USERNAME_MIN}–${USERNAME_MAX} letters, digits, underscores, dots or hyphens.`;
export const PASSWORD_RULE = `Use ${PASSWORD_MIN}–${PASSWORD_MAX} characters.`;

const length = (s: string) => [...s].length;

export type FieldErrors<K extends string> = Partial<Record<K, string>>;

export interface SignupFields {
  username: string;
  password: string;
  invite: string;
}

export function validateSignup({ username, password, invite }: SignupFields): FieldErrors<keyof SignupFields> {
  const errors: FieldErrors<keyof SignupFields> = {};
  const n = length(username);
  if (n < USERNAME_MIN || n > USERNAME_MAX || !USERNAME_PATTERN.test(username)) errors.username = USERNAME_RULE;
  const p = length(password);
  if (p < PASSWORD_MIN || p > PASSWORD_MAX) errors.password = PASSWORD_RULE;
  if (length(invite.trim()) > INVITE_MAX) errors.invite = `Invite codes are at most ${INVITE_MAX} characters.`;
  return errors;
}

export interface LoginFields {
  username: string;
  password: string;
}

export function validateLogin({ username, password }: LoginFields): FieldErrors<keyof LoginFields> {
  const errors: FieldErrors<keyof LoginFields> = {};
  if (username === '') errors.username = 'Enter your username.';
  else if (length(username) > LOGIN_USERNAME_MAX) errors.username = `Usernames are at most ${LOGIN_USERNAME_MAX} characters.`;
  if (password === '') errors.password = 'Enter your password.';
  else if (length(password) > PASSWORD_MAX) errors.password = `Passwords are at most ${PASSWORD_MAX} characters.`;
  return errors;
}
