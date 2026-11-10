// Catches a render or effect error below it and shows an inline message with a retry, so one
// broken chart (or a chart bundle that failed to download) never blanks the whole app.
import { Component, type ReactNode } from 'react';

interface Props {
  /** What to say, e.g. 'Couldn't draw the chart "NDCG@10 by model".' */
  message: string;
  children: ReactNode;
}

interface State {
  failed: boolean;
}

export class ErrorBoundary extends Component<Props, State> {
  state: State = { failed: false };

  static getDerivedStateFromError(): State {
    return { failed: true };
  }

  private retry = () => this.setState({ failed: false });

  render() {
    if (!this.state.failed) return this.props.children;
    return (
      <div className="form-error eval-error" role="alert">
        <p>{this.props.message}</p>
        <button type="button" className="button" onClick={this.retry}>
          Try again
        </button>
      </div>
    );
  }
}
