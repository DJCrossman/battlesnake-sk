import { Test, TestingModule } from '@nestjs/testing';
import { BoundaryService } from './boundary.service';

describe('BoundaryService', () => {
  let service: BoundaryService;

  beforeEach(async () => {
    const module: TestingModule = await Test.createTestingModule({
      providers: [BoundaryService],
    }).compile();

    service = module.get<BoundaryService>(BoundaryService);
  });

  it('should be defined', () => {
    expect(service).toBeDefined();
  });

  describe('moveAsBody', () => {
    const state = {
      game: {} as any,
      turn: 4,
      board: {
        height: 11,
        width: 11,
        snakes: [],
        food: [
          {
            x: 9,
            y: 6,
          },
        ],
        hazards: [],
      },
      you: {
        id: 'gs_4fyGxT7TkcJ9Fcmh6k7bgxGH',
        name: 'David Crossnake',
        latency: '107',
        health: 96,
        body: [
          {
            x: 9,
            y: 5,
          },
          {
            x: 10,
            y: 5,
          },
          {
            x: 10,
            y: 6,
          },
        ],
        head: {
          x: 9,
          y: 5,
        },
        length: 3,
        shout: '',
      },
    };
    it('should be increase length when on food', () => {
      const body = service.moveAsBody('up', state);
      expect(body[0].x).toBe(9)
      expect(body[0].y).toBe(6)
      expect(body.length).toBe(4)
    });
    it('should be move length when not on food', () => {
      const body = service.moveAsBody('down', state);
      expect(body[0].x).toBe(9)
      expect(body[0].y).toBe(4)
      expect(body.length).toBe(3)
    });
  });
});
