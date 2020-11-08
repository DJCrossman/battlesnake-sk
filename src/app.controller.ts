import { Body, Controller, Get, HttpCode, InternalServerErrorException, Post } from '@nestjs/common';
import { AppService } from './app.service';
import { GameState } from './dtos/game-state';
import { Personalization } from './dtos/personalization';
import { SnakeCommand } from './dtos/snake-command';

@Controller()
export class AppController {
  constructor(private readonly appService: AppService) {}

  @Get()
  @HttpCode(200)
  async getSnake(): Promise<Personalization> {
    return this.appService.getSnake();
  }

  @Post('start')
  @HttpCode(200)
  async start(@Body() state: GameState): Promise<void> {
    await this.appService.start(state)
  }

  @Post('move')
  @HttpCode(200)
  async move(@Body() state: GameState): Promise<SnakeCommand> {
    setTimeout(() => new InternalServerErrorException('Too slow'), 500)
    return this.appService.move(state)
  }

  @Post('end')
  @HttpCode(200)
  async end(@Body() state: GameState): Promise<void> {
    await this.appService.end(state)
  }
}
