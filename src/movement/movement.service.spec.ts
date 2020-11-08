import { Test, TestingModule } from '@nestjs/testing';
import { BoundaryService } from '../boundary/boundary.service';
import { GameState } from '../dtos/game-state';
import { MovementService } from './movement.service';

describe('MovementService', () => {
  let service: MovementService;

  beforeEach(async () => {
    const module: TestingModule = await Test.createTestingModule({
      providers: [MovementService, BoundaryService],
    }).compile();

    service = module.get<MovementService>(MovementService);
    service.random = false;
  });

  it('should be defined', () => {
    expect(service).toBeDefined();
  });

  it('should avoid the wall', async done => {
    const state: GameState = {
      game: {} as any,
      turn: 10,
      board: {
        height: 11,
        width: 11,
        snakes: [] as any,
        food: [
          {
            x: 5,
            y: 5,
          },
        ],
        hazards: [],
      },
      you: {
        id: 'gs_yW6S78yVFjGjF9qKd4yFqCkP',
        name: 'David Crossnake',
        latency: '115',
        health: 92,
        body: [
          {
            x: 10,
            y: 10,
          },
          {
            x: 9,
            y: 10,
          },
          {
            x: 9,
            y: 9,
          },
          {
            x: 10,
            y: 9,
          },
        ],
        head: {
          x: 10,
          y: 10,
        },
        length: 4,
        shout: '',
      },
    };
    const { move, weight } = await service.findMove(state, 1);
    expect(move).toBe('down');
    expect(weight).toBeGreaterThan(0);
    done();
  });

  it('should avoid the wall after directly heading for it', async done => {
    const state: GameState = {
      game: {
        id: '4b7c6bd9-caff-4e15-92d9-e50ae8139d48',
        ruleset: {
          name: 'solo',
          version: 'v1.0.13',
        },
        timeout: 500,
      },
      turn: 108,
      board: {
        height: 11,
        width: 11,
        snakes: [
          {
            id: 'gs_FDyy3dc6SpXrB8DRPyjJqHGT',
            name: 'David Crossnake',
            latency: '104',
            health: 88,
            body: [
              {
                x: 6,
                y: 8,
              },
              {
                x: 5,
                y: 8,
              },
              {
                x: 4,
                y: 8,
              },
              {
                x: 3,
                y: 8,
              },
              {
                x: 2,
                y: 8,
              },
              {
                x: 2,
                y: 7,
              },
              {
                x: 2,
                y: 6,
              },
              {
                x: 2,
                y: 5,
              },
              {
                x: 2,
                y: 4,
              },
            ],
            head: {
              x: 6,
              y: 8,
            },
            length: 9,
            shout: '',
          },
        ],
        food: [
          {
            x: 0,
            y: 10,
          },
          {
            x: 9,
            y: 10,
          },
          {
            x: 0,
            y: 7,
          },
          {
            x: 8,
            y: 10,
          },
          {
            x: 0,
            y: 0,
          },
        ],
        hazards: [],
      },
      you: {
        id: 'gs_FDyy3dc6SpXrB8DRPyjJqHGT',
        name: 'David Crossnake',
        latency: '104',
        health: 88,
        body: [
          {
            x: 6,
            y: 8,
          },
          {
            x: 5,
            y: 8,
          },
          {
            x: 4,
            y: 8,
          },
          {
            x: 3,
            y: 8,
          },
          {
            x: 2,
            y: 8,
          },
          {
            x: 2,
            y: 7,
          },
          {
            x: 2,
            y: 6,
          },
          {
            x: 2,
            y: 5,
          },
          {
            x: 2,
            y: 4,
          },
        ],
        head: {
          x: 6,
          y: 8,
        },
        length: 9,
        shout: '',
      },
    };
    const { move, weight } = await service.findMove(state, 1);
    expect(move).toBe('right');
    expect(weight).toBeGreaterThan(0);
    done();
  });

  it('should avoid yourself', async done => {
    const state: GameState = {
      game: {} as any,
      turn: 1,
      board: {
        height: 11,
        width: 11,
        snakes: [] as any,
        food: [
          {
            x: 2,
            y: 8,
          },
          {
            x: 5,
            y: 5,
          },
        ],
        hazards: [],
      },
      you: {
        id: 'gs_34HpMxdhkp9g6P8YJr67HJPY',
        name: 'David Crossnake',
        latency: '261',
        health: 99,
        body: [
          {
            x: 0,
            y: 9,
          },
          {
            x: 1,
            y: 9,
          },
          {
            x: 1,
            y: 9,
          },
        ],
        head: {
          x: 0,
          y: 9,
        },
        length: 3,
        shout: '',
      },
    };
    const { move, weight } = await service.findMove(state, 1);
    expect(move).toBe('down');
    expect(weight).toBeGreaterThan(0);
    done();
  });

  it('should avoid conflict', async (done) => {
    const state: GameState = {
      game: {
        id: '64a4df54-6631-46e6-8010-88a65f5c467f',
        ruleset: {
          name: 'standard',
          version: 'v1.0.13',
        },
        timeout: 500,
      },
      turn: 60,
      board: {
        height: 11,
        width: 11,
        snakes: [
          {
            id: 'gs_3w6HTQYFxv79KwkpFF48K93X',
            name: 'Wascana Willy',
            latency: '11',
            health: 76,
            body: [
              {
                x: 6,
                y: 8,
              },
              {
                x: 6,
                y: 7,
              },
              {
                x: 6,
                y: 6,
              },
              {
                x: 6,
                y: 5,
              },
              {
                x: 7,
                y: 5,
              },
              {
                x: 8,
                y: 5,
              },
              {
                x: 9,
                y: 5,
              },
            ],
            head: {
              x: 6,
              y: 8,
            },
            length: 7,
            shout: '',
          },
          {
            id: 'gs_YtvcJvcJyPR8PTGG3dPgjddW',
            name: 'Wascana Willy',
            latency: '7',
            health: 95,
            body: [
              {
                x: 4,
                y: 8,
              },
              {
                x: 4,
                y: 7,
              },
              {
                x: 4,
                y: 6,
              },
              {
                x: 4,
                y: 5,
              },
              {
                x: 4,
                y: 4,
              },
            ],
            head: {
              x: 4,
              y: 8,
            },
            length: 5,
            shout: '',
          },
        ],
        food: [
          {
            x: 2,
            y: 0,
          },
          {
            x: 3,
            y: 0,
          },
          {
            x: 4,
            y: 10,
          },
          {
            x: 1,
            y: 0,
          },
          {
            x: 8,
            y: 2,
          },
        ],
        hazards: [],
      },
      you: {
        id: 'gs_YtvcJvcJyPR8PTGG3dPgjddW',
        name: 'Wascana Willy',
        latency: '7',
        health: 95,
        body: [
          {
            x: 4,
            y: 8,
          },
          {
            x: 4,
            y: 7,
          },
          {
            x: 4,
            y: 6,
          },
          {
            x: 4,
            y: 5,
          },
          {
            x: 4,
            y: 4,
          },
        ],
        head: {
          x: 4,
          y: 8,
        },
        length: 5,
        shout: '',
      },
    };
    const { move, weight } = await service.findMove(state, 1);
    expect(move).toBe('left');
    expect(weight).toBeGreaterThan(0);
    done();
  });
});
