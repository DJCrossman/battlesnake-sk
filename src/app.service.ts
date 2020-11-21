import { Injectable, Logger } from '@nestjs/common';
import { GameState } from './dtos/game-state';
import { Personalization } from './dtos/personalization';
import { SnakeCommand } from './dtos/snake-command';
import { MovementService } from './movement/movement.service';

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
      color: '#ff69b4',
      head: 'bendr',
      tail: 'freckled',
    };
  }

  public async start(state: GameState): Promise<void> {
    Logger.log(`Starting [${state.game.id}]...`, 'AppService');
  }

  public async move(state: GameState): Promise<SnakeCommand> {
    this.history.push(state);
    Logger.log(`Staring turn [${state.turn}]...`, 'AppService');
    const { move } = await this.moveService.findMove(state, 1);
    Logger.log(`Moving [${move}] on turn ${state.turn}`, 'AppService');
    return {
      move,
    };
  }

  public async end(state: GameState): Promise<void> {
    Logger.log(`Ending [${state.game.id}].`, 'AppService');
  }
}
