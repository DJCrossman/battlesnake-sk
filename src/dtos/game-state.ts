
export interface Ruleset {
  name: string;
  version: string;
}

export interface Game {
  id: string;
  ruleset: Ruleset;
  timeout: number;
}

export interface Coordinate {
  x: number;
  y: number;
}

export interface Snake {
  id: string;
  name: string;
  health: number;
  body: Body[];
  latency: string;
  head: Coordinate;
  length: number;
  shout: string;
  squad: string;
}

export interface Board {
  height: number;
  width: number;
  food: Coordinate[];
  hazards: Coordinate[];
  snakes: Snake[];
}

export interface You {
  id: string;
  name: string;
  health: number;
  body: Coordinate[];
  latency: string;
  head: Coordinate;
  length: number;
  shout: string;
  squad: string;
}

export interface GameState {
  game: Game;
  turn: number;
  board: Board;
  you: You;
}

