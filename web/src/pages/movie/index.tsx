import { useParams } from 'react-router';

export default function MoviePage() {
  const { movieId } = useParams();
  return <h1>Movie {movieId}</h1>;
}
