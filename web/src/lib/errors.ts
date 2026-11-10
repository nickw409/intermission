// Turns any failure from the API client into a short sentence a person can act on.
import { ApiError, type ErrorCode } from '../api/client';

// Record<ErrorCode, …> makes a new contract error code a type error until it gets a message here.
const MESSAGES: Record<ErrorCode, string> = {
  bad_request: 'Please check the form and try again.',
  invalid_json: 'Something went wrong. Please try again.',
  body_too_large: 'That request was too large.',
  unauthorized: 'Please log in to continue.',
  invalid_credentials: 'Incorrect username or password.',
  csrf_rejected: 'Your request was blocked. Reload the page and try again.',
  invalid_invite: "That invite code isn't valid. It may have expired or already been used.",
  username_taken: 'That username is already taken. Please choose another.',
  too_many_ratings: "You've reached the maximum number of ratings.",
  not_found: "We couldn't find that.",
  unknown_model: "That model isn't available.",
  rate_limited: 'Too many attempts. Please wait a minute and try again.',
  model_unavailable: 'Recommendations are temporarily unavailable. Please try again soon.',
  internal: 'Something went wrong. Please try again.',
};

export const NETWORK_MESSAGE = "Can't reach the server. Check your connection and try again.";

export function friendlyMessage(error: unknown): string {
  if (error instanceof ApiError) return MESSAGES[error.code];
  // fetch rejects with a TypeError when the network or server can't be reached.
  if (error instanceof TypeError) return NETWORK_MESSAGE;
  return MESSAGES.internal;
}
