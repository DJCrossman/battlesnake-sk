import { Injectable, Logger } from '@nestjs/common';
import { GameState } from './dtos/game-state';
import { Personalization } from './dtos/personalization';
import { SnakeCommand } from './dtos/snake-command';
import { MovementService } from './movement/movement.service';
import { default as packageJson } from '../package.json';

@Injectable()
export class AppService {
  history: GameState[] = [];
  constructor(private moveService: MovementService) {
    this.history = [];
  }

  public async getSnake(): Promise<Personalization> {
    Logger.log(`Getting snake...`, 'AppService');
    return {
      apiversion: '1',
      author: 'David Crossman',
      color: process.env.SNAKE_COLOUR || '#DD69b4',
      head: 'bendr',
      tail: 'freckled',
      version: packageJson.version,
    };
  }

  public async start(state: GameState): Promise<void> {
    Logger.log(`Starting [${state.game.id}]...`, 'AppService');
    this.history = [];
  }

  public async move(state: GameState): Promise<SnakeCommand> {
    this.history.push(state);
    Logger.log(`Staring turn [${state.turn}]...`, 'AppService');
    const { move } = await this.moveService.findMove(state, 1);
    return {
      move,
    };
  }

  public async end(state: GameState): Promise<void> {
    Logger.log(`Ending [${state.game.id}].`, 'AppService');
    const isWin = state.you.id === state.board.snakes[0].id;
    Logger.log(`Game ended with ${isWin ? 'win' : 'loss'}.`, 'AppService', {
      moves: this.history.length,
      weights: this.moveService.WEIGHTS,
      version: packageJson.version,
    });
  }
}
